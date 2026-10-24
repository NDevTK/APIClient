/* bridge.js — the host bridge, the trusted-zone JS edge of the engine (SECURITY.md).
 *
 * The engine is the untrusted WASM, so this file is what it cannot be: it drives the qjs_step protocol,
 * fetches replies and chunks through the safeFetch chokepoint, persists the cross-session frontier to
 * IndexedDB, and JSON.parses the engine's one @RESULT document. It installs self.astDispatch and
 * self.kickHostPool for the offscreen document. It holds no analysis logic: identity, dedup and detection
 * belong to the C engine.
 *
 * No engine lives in this realm. Each instance is a renderer that renderer-host.js provisions in an
 * `<iframe sandbox="allow-scripts">` with a unique opaque origin, and every qjs_* call is an awaited method of
 * `content.mojom.Renderer`, validated at both ends of a MessagePort. Every ABI call is therefore a suspension
 * point at which another engine's round can interleave: two calls in a row are atomic only if written to be. */
// The root document name for each engine this offscreen owns (see qjs_init below). A document an engine
// creates (a child navigable, a popup) names itself "<parent>.<k>", so HTML §4.8.5 The iframe element can
// create a child navigable inside the insertion steps without asking this zone.
let nextDocumentId = 0;
/* The run log and the crash count, declared at load so that their absence means this file did not load, an
   empty log means no engine finalized, and a zero means no engine aborted. crashBanner counts every abort
   path; rendererPoolProbe reads both, and the log also answers the diagnostics page's GET_ENGINE_RUNS. The log holds page
   addresses, so hostClear empties it; the crash count holds none and survives a clear.
   One row per run: a run owns exactly one row (`eng._log`), a partial snapshot overwrites that row rather than
   appending, and the record's `run` field says which state the row reports, so a snapshot of a run in
   progress is never read as a finished run. */
self._engineLog = [];
self._engineCrashOccurred = 0;
/* The Level-1 order: the reading `_level1Record` writes at the end of every scheduler round. Level 1 (the host
   ordering live per-page engines by best-flow weight) is composed entirely in this zone, since no engine can
   see another, so no result document can carry it. The record carries extrema and populations across the
   ranked set rather than only the pick, because a rank frozen at a constant is visible only in the spread.
   Declared at load for three states: `undefined` is this file not loaded, `null` is no round completed this
   session, an object is a reading. A clear does not touch it: it holds no address or identity, and the
   round after a clear records the emptied pool. */
self._level1 = null;

/* Whether a URL has an opaque hole, so it is not concretely fetchable (gates reply and chunk fetch). Asked
   through lib/callsite-url.js in endpoint.c's own hole grammar, so every hole shape the engine emits is
   recognised. Endpoint identity (hole normalization, shape/concrete collapse) belongs to the engine. */
const hasHole = (s) => astAddressHasHole(s || "");

/* Map the engine's one `@RESULT <json>` line to the analysis object the offscreen consumes. The engine builds
   and dedups the whole result (endpoints, params, headers, bodies, @S sinks, page errors, park recipes); this
   zone does one JSON.parse and relays. @E lines (host-side protocol errors) are surfaced so a zero result
   never fails silently.
   `outcome` is the caller's statement of what this record is, and travels on the result as `_run`:
     "partial"        — a mid-run snapshot (qjs_emit_partial); a document is required.
     "complete"       — the run ended and the engine answered with its result document; a document is required.
     "crashed"        — the instance aborted; usually no document, but one is present when the abort landed
                        after a partial was printed and before this zone consumed it, and it is asserted as usual.
     "nothing-to-run" — no HTML and no code, so no engine ran; no document, and not a crash.
   A crash invalidates the run's claims of completeness, not its observations: findings composed before the
   abort are already merged into the cumulative record and are still relayed. The cost counters, the park
   residue and the right to overwrite the cross-session frontier entry are gated on `_run` at their sites. */
/* `witness` on each @H row, per solver/endpoint.h: one element per distinct sighting witness, `null` where no
   flow stood, else `{assumed, verification}`: the sighting's entries — `{source, value, kind}` per pin,
   `{decision, taken}` per assumed branch, `{decision, completion}` per assumed native completion — and one of
   WITNESS_VERIFICATIONS. A row from an engine build that predates the
   key carries none, and then no row does (one instance emits one document shape). Beside `witnessClass`:
   `unasked` is exactly the row whose every element is `null` (the class is a MAX over the same sightings), and
   a `may-rest-on` row has a sighting whose path pinned a value, so some element holds a pin. */
const WITNESS_PIN_KINDS = Object.freeze(["string", "number", "boolean", "null", "undefined", "bigint"]);
const WITNESS_VERIFICATIONS = Object.freeze(["no-candidate", "candidate", "verified"]);
/* The result document is the engine's statement and so input to this zone (in the @S half it is also the page's
   output). linesToAnalysis refuses a whole document that fails any check below and records the run as crashed
   with the reason, so nothing downstream reads a field these did not establish. Each checker answers null for a
   value of its shape and otherwise the reason, naming the field; a document's reason is its first failure. */
const docIsRecord = (v) => !!v && typeof v === "object" && !Array.isArray(v);
const docIsCount = (v) => typeof v === "number" && Number.isFinite(v);
const docIsName = (v) => typeof v === "string" && v !== "";
function docObject(v, f) { return docIsRecord(v) ? null : f + " is not an object"; }
function docArray(v, f) { return Array.isArray(v) ? null : f + " is not an array"; }
function docCount(v, f) { return docIsCount(v) ? null : f + " is not a finite number (" + String(v) + ")"; }
function docEach(xs, f, check) {
  for (let i = 0; i < xs.length; i++) {
    const why = check(xs[i], f + "[" + i + "]");
    if (why !== null) return why;
  }
  return null;
}
/* One witness entry, one of solver/endpoint.h's three shapes: a pin, a branch decision, an outcome decision. */
function docWitnessEntry(x, f) {
  if (!docIsRecord(x)) return f + " is not an object";
  const keys = Object.keys(x).sort().join(",");
  if (keys === "kind,source,value")
    return docIsName(x.source) && typeof x.value === "string" && WITNESS_PIN_KINDS.indexOf(x.kind) >= 0 ? null
      : f + " is not a pin {source, value, kind} with a non-empty source, a string value and one of " +
        WITNESS_PIN_KINDS.join("/");
  if (keys === "decision,taken")
    return docIsName(x.decision) && typeof x.taken === "boolean" ? null
      : f + " is not a branch decision {decision, taken}";
  return keys === "completion,decision" && docIsName(x.decision) && Number.isInteger(x.completion) &&
         x.completion >= 0 ? null : f + " is none of a pin, a branch decision or an outcome decision";
}
/* One @H row's `witness`, against the row's own `witnessClass`: `unasked` is exactly the row no flow stood at,
   and `may-rest-on` has a sighting holding a pin (both read off one set of sightings). */
function docWitness(row, f) {
  const w = row.witness;
  if (!Array.isArray(w) || w.length === 0) return f + ".witness is not a non-empty array";
  let asked = 0, pinned = 0;
  for (let j = 0; j < w.length; j++) {
    const el = w[j], g = f + ".witness[" + j + "]";
    if (el === null) continue;
    if (!docIsRecord(el) || Object.keys(el).sort().join(",") !== "assumed,verification" ||
        !Array.isArray(el.assumed) || WITNESS_VERIFICATIONS.indexOf(el.verification) < 0)
      return g + " is neither null nor {assumed: [...], verification: " + WITNESS_VERIFICATIONS.join("|") + "}";
    if (el.verification !== "no-candidate" && el.assumed.length === 0)
      return g + " assumed nothing and still names a verification run";
    asked++;
    const why = docEach(el.assumed, g + ".assumed", docWitnessEntry);
    if (why !== null) return why;
    pinned += el.assumed.filter((x) => "source" in x).length;
  }
  if ((row.witnessClass === "unasked") !== (asked === 0))
    return f + " states witnessClass " + JSON.stringify(row.witnessClass) + " beside " + asked + " asked witness(es)";
  if (row.witnessClass === "may-rest-on" && pinned === 0)
    return f + " states may-rest-on and no witness holds a pin";
  return null;
}
/* The per-row facts solver/endpoint.c writes as tokens, and `addressRoot`, whose domain is null, false or a
   non-empty string. A key is stated on every row or on none, since one instance emits one document shape. */
const ENDPOINT_FACT_KEYS = Object.freeze(["door", "mintedAt", "addressClass", "razorClass", "witnessClass"]);
function docEndpointRow(row, f) {
  if (!docIsRecord(row)) return f + " is not an object";
  for (const k of ENDPOINT_FACT_KEYS)
    if (k in row && !docIsName(row[k])) return f + "." + k + " is not a non-empty string";
  if ("addressRoot" in row && row.addressRoot !== null && row.addressRoot !== false && !docIsName(row.addressRoot))
    return f + ".addressRoot is none of null, false or a non-empty string";
  return "witness" in row ? docWitness(row, f) : null;
}
function docEndpoints(rows) {
  const why = docArray(rows, "fetchCallSites") || docEach(rows, "fetchCallSites", docEndpointRow);
  if (why !== null) return why;
  for (const k of [...ENDPOINT_FACT_KEYS, "addressRoot", "witness"]) {
    const stated = rows.filter((r) => k in r).length;
    if (stated !== 0 && stated !== rows.length)
      return "fetchCallSites states `" + k + "` on " + stated + " of " + rows.length + " rows";
  }
  /* solver/endpoint.c writes `addressRoot: null` on exactly the rows whose `addressClass` is `concrete`. */
  if (rows.length && "addressRoot" in rows[0] && "addressClass" in rows[0]) {
    const nc = rows.filter((r) => r.addressRoot === null).length;
    const cc = rows.filter((r) => r.addressClass === "concrete").length;
    if (nc !== cc) return "fetchCallSites has " + nc + " rows with no delivery root against " + cc + " concrete";
  }
  return null;
}
/* One @S entry's delivery facts, as solver/solve.c's emit_delivery writes them (solve.h's field list): `delivery`
   is absent (the source declares none) or one of the five mechanism tokens, and `deliveryPrefix` is the
   victim's address component, present exactly on an `address` delivery; `principalDemand` is absent
   (unexamined) or one of three verdicts, with `principalGates` only beside `forgeable` (and on every one) and
   `principalPins` exactly beside `unforgeable`; `poc` is absent on a parked search and a non-empty string on a
   fire-verified one, and `senderDemand` (the senders a parked search waits on, in the principalPins row
   grammar) only on a parked one. These are what the live-verify layer (offscreen-brain.js) performs a delivery
   from. */
const SINK_DELIVERIES = Object.freeze(["address", "plant", "referring-address", "user-file", "cross-document-message"]);
const SINK_DEMANDS = Object.freeze(["none", "forgeable", "unforgeable"]);
function docSinkGate(g, f) {
  if (!docIsRecord(g) || !docIsName(g.principal) || !Array.isArray(g.predicates) || g.predicates.length === 0)
    return f + " is not {principal, predicates: [...]} with a principal and at least one predicate";
  return docEach(g.predicates, f + ".predicates", (pr, h) =>
    docIsRecord(pr) && docIsName(pr.method) && Array.isArray(pr.arguments) &&
    pr.arguments.every((a) => typeof a === "string") && typeof pr.holds === "boolean"
      ? null : h + " is not {method, arguments: [strings], holds: boolean}");
}
function docSinkPin(p, f) {
  return docIsRecord(p) && docIsName(p.principal) && typeof p.equals === "string" && docIsName(p.kind) &&
         typeof p.loose === "boolean" && (!("subject" in p) || typeof p.subject === "string")
    ? null : f + " is not {principal, equals, kind, loose[, subject]}";
}
function docSink(e, f) {
  if (!docIsRecord(e)) return f + " is not an object";
  if ("delivery" in e && SINK_DELIVERIES.indexOf(e.delivery) < 0)
    return f + ".delivery " + JSON.stringify(e.delivery) + " is none of " + SINK_DELIVERIES.join(", ");
  if (("deliveryPrefix" in e) !== (e.delivery === "address"))
    return f + " states a deliveryPrefix exactly where its delivery is not `address`";
  if ("deliveryPrefix" in e && e.deliveryPrefix !== "#" && e.deliveryPrefix !== "?")
    return f + ".deliveryPrefix " + JSON.stringify(e.deliveryPrefix) + " is neither # nor ?";
  if ("principalDemand" in e && SINK_DEMANDS.indexOf(e.principalDemand) < 0)
    return f + ".principalDemand " + JSON.stringify(e.principalDemand) + " is none of " + SINK_DEMANDS.join(", ");
  if (e.principalDemand === "forgeable" ? !("principalGates" in e) : ("principalGates" in e && "principalDemand" in e))
    return f + " states principalGates on a verdict other than forgeable, or a forgeable verdict without them";
  if (("principalPins" in e) !== (e.principalDemand === "unforgeable"))
    return f + " states principalPins on a verdict other than unforgeable, or an unforgeable verdict without them";
  let why = null;
  if ("principalGates" in e)
    why = (Array.isArray(e.principalGates) && e.principalGates.length > 0 ? null : f + ".principalGates is not a non-empty array") ||
          docEach(e.principalGates, f + ".principalGates", docSinkGate);
  if (why === null && "principalPins" in e)
    why = (Array.isArray(e.principalPins) && e.principalPins.length > 0 ? null : f + ".principalPins is not a non-empty array") ||
          docEach(e.principalPins, f + ".principalPins", docSinkPin);
  if (why === null && "poc" in e && !docIsName(e.poc)) why = f + ".poc is not a non-empty string";
  if (why === null && "senderDemand" in e)
    why = ("poc" in e ? f + " states a senderDemand on a fire-verified entry, which only a parked search carries" : null) ||
          (Array.isArray(e.senderDemand) && e.senderDemand.length > 0 ? null : f + ".senderDemand is not a non-empty array") ||
          docEach(e.senderDemand, f + ".senderDemand", docSinkPin);
  return why;
}
function docRoutedPost(p, f) {
  return docIsRecord(p) && typeof p.id === "string" && /^[0-9a-f]{16}$/.test(p.id) && typeof p.target === "string" &&
         typeof p.targetOrigin === "string" && typeof p.relation === "string" && Array.isArray(p.symbols) &&
         p.symbols.length > 0 && p.symbols.every((s) => docIsRecord(s) && typeof s.source === "string" &&
           typeof s.shape === "string" && typeof s.path === "string")
    ? null : f + " is not {id: 16 hex, target, targetOrigin, relation, symbols: [{source, shape, path}, ...]}";
}
/* A census row is a finite number or a histogram whose arms are finite numbers or positional tuples of them
   (solver/result.c composes each `childRealmRefSites` arm as `[min, max, total]`). */
function docCensusRow(v, f) {
  if (docIsCount(v)) return null;
  if (!docIsRecord(v)) return f + " is neither a finite number nor a histogram";
  for (const a of Object.keys(v)) {
    const arm = v[a];
    if (Array.isArray(arm) ? !arm.every(docIsCount) : !docIsCount(arm))
      return f + "." + a + " is neither a finite number nor a tuple of them";
  }
  return null;
}
function docCensus(c, f, nonEmpty) {
  const why = docObject(c, f);
  if (why !== null) return why;
  if (nonEmpty && Object.keys(c).length === 0) return f + " is empty";
  for (const k of Object.keys(c)) {
    const w = docCensusRow(c[k], f + "." + k);
    if (w !== null) return w;
  }
  return null;
}
/* `members` is always present; term rows appear exactly when it is above zero. */
function docWfq(w) {
  const why = docObject(w, "_wfq");
  if (why !== null) return why;
  if (!Number.isInteger(w.members) || w.members < 0) return "_wfq.members is not a count";
  if ((Object.keys(w).length > 1) !== (w.members > 0))
    return "_wfq reports " + w.members + " members with " + (Object.keys(w).length - 1) + " term rows";
  for (const k of Object.keys(w)) if (!docIsCount(w[k])) return "_wfq." + k + " is not a finite number";
  return null;
}
function docQuantum(q) {
  return docObject(q, "_quantum") ||
    (Number.isSafeInteger(q.sliceWork) && q.sliceWork > 0 ? null : "_quantum.sliceWork is not a positive integer") ||
    (Number.isSafeInteger(q.hostYieldMs) && q.hostYieldMs >= 0 ? null : "_quantum.hostYieldMs is not a count") ||
    (docIsName(q.price) && typeof q.priceIsCpu === "boolean" ? null
      : "_quantum carries no `price` string or boolean `priceIsCpu`");
}
/* The families of solver/metrics.h's registry, from the schema the document carries beside them: every declared
   row is a number on the document and every declared identity holds, so this zone keeps no row list of its own.
   The @S arrival census's four rows are also read by name (linesToAnalysis), so they must be declared. */
const METRIC_KINDS = ["lifetime", "gauge", "maximum", "constant", "regime"];
const SCENSUS_READ = ["_sourceReads", "_sinkReached", "_sinkTainted", "_sinkNeedsSender"];
function docMetricsFamilies(r) {
  const s = r._metricsSchema;
  if (!docIsRecord(s) || !Array.isArray(s.families)) return "_metricsSchema carries no `families` array";
  for (const fam of s.families) {
    if (!docIsRecord(fam) || typeof fam.family !== "string" || !Array.isArray(fam.rows) || fam.rows.length === 0 ||
        !Array.isArray(fam.identities))
      return "_metricsSchema has a family with no name, no rows or no identities";
    for (const row of fam.rows) {
      if (!docIsRecord(row) || typeof row.key !== "string" || !METRIC_KINDS.includes(row.kind) ||
          typeof row.unit !== "string" || typeof row.scope !== "string" || typeof row.owner !== "string")
        return "_metricsSchema's `" + fam.family + "` family declares a row without key, kind, unit, scope and owner";
      const why = docCount(r[row.key], row.key + " (declared in the `" + fam.family + "` family)");
      if (why !== null) return why;
    }
    for (const e of fam.identities)
      if (!docIsRecord(e) || e.rel !== "le" || !(r[e.sub] <= r[e.sup]))
        return "the `" + fam.family + "` family declares " + String(e && e.sub) + " <= " + String(e && e.sup) +
               " and the document reports " + String(e && r[e.sub]) + " and " + String(e && r[e.sup]);
  }
  const sc = s.families.find((fam) => fam.family === "scensus");
  for (const k of SCENSUS_READ)
    if (!sc || !sc.rows.some((row) => row.key === k)) return "_metricsSchema's `scensus` family does not declare " + k;
  return null;
}
/* The cost counters result.c emits in one composition; the registry families are checked from the schema. A
   missing one also means a wasm built before the field was added: read extension/lib/qjs/qjs.mjs.build.json's
   `head` before reading it as a composition change. */
const RESULT_COUNTERS = Object.freeze(["_switches", "_flows", "_candidates", "_jobsQueued", "_jobsRun",
  "_unitsDone", "_worldSegmentsHeld", "_worldSegmentsMade", "_worldSegmentsForked", "_routedDelivered",
  "_routedRefused", "_routedTasksFired", "_routedTasksTargetOrigin", "_routedTasksTargetGone", "_routedTasksThrew",
  "_routedTasksRetracted",
  "_orphansDriven", "_orphansAsked", "_orphanAskMemo", "_orphanAskEmpty", "_orphanAskTook", "_orphanWalks",
  "_orphanWalkEntries", "_orphanWalksFull", "_orphanWalkFullCandidates"]);
/* The whole contract solver/result.c composes, or the first field that breaks it. `complete` adds the session's
   close: an empty `_park` means a drained frontier (`_cold.live` 0), because `finish` turns an empty park into a
   delete of this origin's cross-session entry. */
function resultDocumentRefusal(r, complete) {
  let why = docObject(r, "the @RESULT document") || docEndpoints(r.fetchCallSites) ||
    docArray(r.securitySinks, "securitySinks") || docEach(r.securitySinks, "securitySinks", docSink) ||
    docArray(r.pageErrors, "pageErrors") ||
    docArray(r.pageErrorsRetracted, "pageErrorsRetracted") || docArray(r.pageErrorsExplored, "pageErrorsExplored") ||
    docArray(r.routedPosts, "routedPosts") || docEach(r.routedPosts, "routedPosts", docRoutedPost);
  if (why !== null) return why;
  for (const k of RESULT_COUNTERS) if ((why = docCount(r[k], k)) !== null) return why;
  why = docMetricsFamilies(r) || docWfq(r._wfq) || docQuantum(r._quantum);
  if (why !== null) return why;
  /* `_forkAt` lists predicates that forked, so `{}` is a document that never forked; the other four read state
     that exists at every instant. */
  for (const k of ["_cold", "_heap", "_swap", "_forkAt", "_absent"])
    if ((why = docCensus(r[k], k, k !== "_forkAt")) !== null) return why;
  why = docArray(r._park, "_park");
  if (why !== null) return why;
  if (["refused", "requested", "none"].indexOf(r._parkWhy) < 0 || (r._parkWhy === "none") !== (r._park.length === 0))
    return "_parkWhy is " + JSON.stringify(r._parkWhy) + " beside " + r._park.length + " recipe(s)";
  if (complete) {
    if (!docIsCount(r._cold.live)) return "_cold.live is not a finite number";
    if (r._park.length === 0 && r._cold.live !== 0)
      return "a complete run closed over " + r._cold.live + " live member(s) and wrote no park recipes";
  }
  return null;
}
const RUN_OUTCOMES = ["partial", "complete", "crashed", "nothing-to-run"];
/* Writes the run's own log row. `eng` is the run's identity and is absent for records that belong to no
   instance: nothing to run (which logs nothing) and a boot that aborted before the reservation had an
   instance, whose record is built per waiting caller. A row is written in place, keeping the position the run
   started at; a row already shifted out of the log is no longer displayed, so writing it is a harmless no-op
   where an index would have addressed another run.
   Fields are replaced, not merged: a crash record carries no counters, and `Object.assign` alone would leave
   a partial's numbers under a row labelled crashed. */
function engineLogWrite(eng, m) {
  const row = eng ? eng._log : null;
  if (!row) {
    if (eng) eng._log = m;
    self._engineLog.push(m);
    if (self._engineLog.length > 200) self._engineLog.shift();
    return;
  }
  for (const k of Object.keys(row)) delete row[k];
  Object.assign(row, m);
}
/* The one reader of `_resumed`, keeping its three states apart: `null` engine (nothing-to-run) reports null; an
   engine reports the count `begin` wrote; an engine whose boot died before `begin` carries its reservation's
   `null`. The DCHECK keeps `undefined` from becoming a fourth state rendered as absence. */
function engineResumed(eng) {
  if (eng === null) return null;
  DCHECK(eng && typeof eng === "object",
         "an analysis was composed against an engine record that is neither an instance nor the stated " +
         "absence — `null` is how this seam's callers say 'no instance ran', and any other falsy value is a " +
         "caller that stopped passing one rather than one saying there was none");
  DCHECK(eng._resumed === null ||
         (typeof eng._resumed === "number" && Number.isInteger(eng._resumed) && eng._resumed >= 0),
         "an engine record carries a resume count that is neither a count nor the stated 'not known' (`" +
         String(eng._resumed) + "`) — engineReserve declares it null and engineRoot writes it once at `begin`, " +
         "so anything else is a third writer, and the value it wrote would be rendered to the user as a " +
         "number of parked flows that came back");
  return eng._resumed;
}
/* The one reader of the cold-tier lookup, on `engineResumed`'s shape: no engine reports the stated absence, an
   engine that reached its frontier read reports what it met, and one whose boot died first carries its
   reservation's nulls unchanged. The three fields are returned together because each is unreadable without
   the others: the bundle id is the half of the frontier key not already on the row as `url`, which is what
   makes two drives of one address comparable. */
function engineColdLookup(eng) {
  if (eng === null) return { lookup: null, other: null, bundle: null };
  DCHECK(eng && typeof eng === "object",
         "an analysis was composed against an engine record that is neither an instance nor the stated " +
         "absence while its cold-tier lookup was being read — `null` is how this seam's callers say 'no " +
         "instance ran', and any other falsy value is a caller that stopped passing one");
  DCHECK(eng._coldLookup === null || COLD_LOOKUP.indexOf(eng._coldLookup) >= 0,
         "an engine record carries a cold-tier lookup this seam does not speak (`" + String(eng._coldLookup) +
         "`) — engineReserve declares it null and engineRoot writes one of a closed set of words once, so " +
         "anything else is a second writer, and the reader is shown a reason for a miss that nothing decided");
  /* The two nulls are written on one line by engineRoot and must stay one fact. */
  DCHECK((eng._bundleId === null) === (eng._coldLookup === null),
         "an engine record states a bundle id (`" + String(eng._bundleId) + "`) and a cold-tier lookup (`" +
         String(eng._coldLookup) + "`) that disagree about whether the frontier was ever read — engineRoot " +
         "writes both on one line at the one moment either exists");
  DCHECK(eng._bundleId === null || (typeof eng._bundleId === "string" && eng._bundleId !== ""),
         "an engine record carries a bundle id that is neither a base-36 identifier nor the stated absence " +
         "(`" + String(eng._bundleId) + "`) — it is half the frontier key, and the half a reader needs to " +
         "tell a redeployed bundle from a cold tier that lost an entry");
  /* The count is present exactly where keys were read: `not-asked` and `unreadable` read none, so a zero from
     them would falsely claim this address holds no other entries (which is what `unvisited` says). */
  const counted = eng._coldLookup !== null && eng._coldLookup !== "not-asked" && eng._coldLookup !== "unreadable";
  DCHECK(counted === (eng._coldOther !== null),
         "an engine record reports a cold-tier lookup of `" + String(eng._coldLookup) + "` with a sibling " +
         "count of `" + String(eng._coldOther) + "` — the two arms that read no keys state the absence of a " +
         "count and every other arm states one, and a zero from an arm that never looked is the claim this " +
         "address has nothing else parked at it");
  DCHECK(eng._coldOther === null ||
         (typeof eng._coldOther === "number" && Number.isInteger(eng._coldOther) && eng._coldOther >= 0),
         "an engine record carries a sibling-entry count that is neither a count nor the stated absence of " +
         "one (`" + String(eng._coldOther) + "`) — it is how many entries this document's address holds under " +
         "OTHER bundle ids, which is the evidence under the word beside it");
  return { lookup: eng._coldLookup, other: eng._coldOther, bundle: eng._bundleId };
}
/* The emitted surface partitioned by a fact each @H row states about itself, over the same `fetchCallSites`
   array whose length is `endpoints` on the run record, so partition and total are one population at one
   moment. solver/endpoint.c writes `door`, `mintedAt` and the other class keys per row; the histogram is taken
   here, not in lib/merge.js, because that loop skips structural and non-network rows and folds rows by
   `method+host+path`, while a door is a per-sighting fact the producer refuses to fold.
   An absent key is its own bucket, `(unstated)`: an artifact predating the key is ordinary because this JS
   ships before the engine is rebuilt. The parentheses keep it from colliding with producer tokens.
   Values are not checked against a copied vocabulary: the door list grows with endpoint_record's call sites,
   so a copy here would abort the trusted zone on a correct new door. Only a non-empty string is asserted.
   A null-prototype map is used so an engine key spelled `constructor` or `__proto__` cannot read
   `Object.prototype`. */
const ENDPOINT_FACT_UNSTATED = "(unstated)";
function endpointFactHistogram(rows, key) {
  const h = Object.create(null);
  for (let i = 0; i < rows.length; i++) {
    /* `in`, never `||`: an absent key is a fact about the build, a present one a fact about the address. */
    let v;
    if (!(key in rows[i])) v = ENDPOINT_FACT_UNSTATED;
    else {
      DCHECK(typeof rows[i][key] === "string" && rows[i][key] !== "",
             "an @H row carries `" + key + ": " + JSON.stringify(rows[i][key]) + "` — solver/endpoint.c " +
             "writes both of these keys through a function returning one of a fixed table of C string " +
             "literals, so anything that is not a non-empty string is that emission and this reader having " +
             "parted, and the row would partition under a key nothing can be read as");
      v = rows[i][key];
    }
    h[v] = (h[v] === undefined ? 0 : h[v]) + 1;
  }
  return Object.assign({}, h);
}
/* The same partition over `addressRoot`, as a separate reader for two reasons. Its domain is three JSON types,
   not a token: solver/endpoint.c emits `null` where no sighting held a concolic address, `false` where one did
   and its bytes entered through no engine-minted source, and a non-empty string naming the sources otherwise.
   And the string is page-influenced (solver/absent.c mints source names from page property names), so it may
   not be a histogram key: `Object.assign({}, h)` copies with [[Set]] and a `__proto__` key would set a
   prototype. The names stay on the row; this counts the four-way state under fixed parenthesised keys.
   It is a diagnostic, not an operand of the bar: whose unknown a root names is not decidable from the string. */
const ENDPOINT_ROOT_NAMED = "(named)";
const ENDPOINT_ROOT_UNATTRIBUTED = "(unattributed)";
const ENDPOINT_ROOT_NO_CONCOLIC = "(no-concolic)";
function endpointAddressRootHistogram(rows) {
  const h = Object.create(null);
  for (let i = 0; i < rows.length; i++) {
    let v;
    if (!("addressRoot" in rows[i])) v = ENDPOINT_FACT_UNSTATED;
    else if (rows[i].addressRoot === null) v = ENDPOINT_ROOT_NO_CONCOLIC;
    else if (rows[i].addressRoot === false) v = ENDPOINT_ROOT_UNATTRIBUTED;
    else {
      DCHECK(typeof rows[i].addressRoot === "string" && rows[i].addressRoot !== "",
             "an @H row carries `addressRoot: " + JSON.stringify(rows[i].addressRoot) + "` — solver/endpoint.c "
             + "writes this key in one of exactly three JSON types (`null`, `false`, or the delivery root as a "
             + "non-empty string), so a fourth is that emission and this reader having parted, and the row "
             + "would partition under a state nothing can be read as");
      v = ENDPOINT_ROOT_NAMED;
    }
    h[v] = (h[v] === undefined ? 0 : h[v]) + 1;
  }
  return Object.assign({}, h);
}
/* THE ROUTED-POST JOIN, this zone's half of engine/trusted.mjs's `@JOIN`, in the same record shape. A receiver
   mints its whole routed `data` as `message.data:<id>`; the sender's `routedPosts` (solver/post_index.h) names
   its own source at each member path under that id. Every arriving document's posts are kept here under their
   id with the sender origin this zone stamps from its own record of the instance (the engine may not state its
   own), and each document's findings over `message.data:<id>[.path]` are joined on the run row. A receiver path
   is covered by the sender symbol at its longest prefix, or holds the symbols below it (`contains`). An id no
   sender has indexed yet reads `sender: null`: a message holding no unknown is never indexed, and a sender's
   posts arrive with its next result document, which may come after the receiver's row. So taking a post
   completes every logged `sender: null` record under its id; whichever half arrives second makes the join. The
   map is this zone's in-memory record of what instances emitted, as `_remoteOps` is. */
const _routedPosts = new Map();
function routedPostsTake(eng, msg, result) {
  for (const p of result.routedPosts)
    _routedPosts.set(p.id, { sender: msg.sourceUrl, senderOrigin: stampOrigin(eng.origin), post: p });
  if (!result.routedPosts.length) return;
  for (const row of self._engineLog) {
    if (!Array.isArray(row.routedJoins)) continue;
    row.routedJoins = row.routedJoins.map((j) => j.sender === null && _routedPosts.has(j.id)
                                                   ? routedJoinOf(j.receiver, j.id, j.path) : j);
  }
}
/* Whether member path `p` is `q` or lies below it, segment by segment (".qq" is not below ".q"). */
function routedPathUnder(p, q) { return p === q || p.startsWith(q + "."); }
/* One join record. */
function routedJoinOf(receiver, id, path) {
  const s = _routedPosts.get(id);
  if (!s) return { receiver, id, path, sender: null };
  const sym = s.post.symbols.filter((x) => routedPathUnder(path, x.path))
                            .sort((a, b) => b.path.length - a.path.length)[0] || null;
  const contains = sym ? [] : s.post.symbols.filter((x) => routedPathUnder(x.path, path))
                                             .map((x) => ({ path: x.path, source: x.source }));
  return { receiver, id, path, sender: s.sender, senderOrigin: s.senderOrigin, relation: s.post.relation,
           targetOrigin: s.post.targetOrigin, source: sym ? sym.source : null, shape: sym ? sym.shape : null,
           contains };
}
function routedPostJoins(msg, result) {
  const ref = /message\.data:([0-9a-f]{16})\}?((?:\.[^\s"{}\[\].]+)*)/g;
  const text = JSON.stringify([result.fetchCallSites, result.securitySinks]);
  const seen = new Set();
  const out = [];
  for (const m of text.matchAll(ref)) {
    /* A derivation over a member renders as a call segment after it (`.encodeURIComponent()`, solver/
       concolic.c's "%s.%s()" shape), so the member path is the segments before the first call. */
    const call = m[2].search(/\.[^.]*\(/);
    const path = call < 0 ? m[2] : m[2].slice(0, call);
    const key = m[1] + " " + path;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(routedJoinOf(msg.sourceUrl, m[1], path));
  }
  return out;
}

function linesToAnalysis(lines, msg, outcome, eng) {
  DCHECK(RUN_OUTCOMES.indexOf(outcome) >= 0,
         "a run outcome this seam does not speak: `" + outcome + "` — every consumer of an analysis branches " +
         "on `_run`, so a word none of them knows is a run whose completeness nothing can judge");
  /* The document this analysis is about. lib/merge.js resolves relative call-site addresses against
     `sourceUrl` and keys each security finding on it, so it is asserted, never defaulted. engineRoot asserts the
     same field non-empty and `eng.msg === msg` where the instance is rooted; both callers pass `eng.msg`. */
  DCHECK(!!msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "an analysis is being built for a document with no address (`" + (msg && msg.sourceUrl) + "`) — " +
         "engineRoot asserts this same field non-empty before the instance runs, so its absence here is that " +
         "message record having been replaced, and every finding on this analysis would be keyed on the " +
         "empty string and land in the moat under a fabricated name");
  let result = null;
  const extraErrors = [];
  /* The resume count is a fact about the session, read off the record (`eng._resumed`, written once by
     engineRoot at `begin`), never off `lines`: callers pass different slices of output (`finish` the whole run,
     `streamPartial` one @RESULT, `crashRecord` a synthetic line), and only the first holds `@RESUMED`. A
     session that never reached `begin` carries `null`, which this function cannot distinguish itself. */
  const resumed = engineResumed(eng);
  /* The cold-tier lookup is also a fact about how the session began, so it is reported on both record shapes;
     a crashed run's reader needs it most. */
  const cold = engineColdLookup(eng);
  /* The cause of a crash. Both producers of a crashed record (engineCrash, with the root @WHY appended, and
     crashRecord for a boot that never got an instance) put an `@E {"phase":"engine-crash",…,"err":…}` line in
     `lines` before calling here. */
  let crashErr = "";
  /* Why this zone refused the engine's document, or null. The document is the engine's statement and so input:
     one that does not parse, is not a record carrying every field consumers read, or is absent from a run that
     owes one is refused, and the run is recorded as crashed with the refusal as its cause, never as a page that
     was analysed and found clean. */
  let refusal = null;
  for (const raw of lines) {
    const ln = String(raw);
    if (ln.startsWith("@RESULT ")) {
      try { result = JSON.parse(ln.slice(8)); }
      catch (e) { result = null; refusal = "the engine's @RESULT line is not JSON: " + String(e && e.message || e); }
    } else if (ln.startsWith("@E ")) {
      extraErrors.push({ context: "engine", message: ln.slice(3) });
      /* Parsed, not substring-matched: the `phase` field identifies the crash line, so a page's own error
         text cannot be mistaken for it. Non-JSON `@E` lines are other diagnostics, recorded above. */
      try {
        const o = JSON.parse(ln.slice(3));
        if (o && o.phase === "engine-crash" && typeof o.err === "string") {
          crashErr = o.err;
          /* engineRefuse's line: an instance answer this zone refused, recorded as the document's refusal is. */
          if (o.stage === "result-refused") extraErrors.push({ context: "result-refused", message: o.err });
        }
      } catch (_) { /* not the crash line; it is already recorded as an engine error above */ }
    } else if (ln.startsWith("@WHY ")) {
      /* An engine diagnostic for a zero-result or resource path, surfaced so no flow fails silently. Two
         producers: engine/host/check.h emits a JSON record (phase/cond/at/reason, escaped at the emitter), and
         engine/qjs/quickjs-check.h, which cannot include the host header, emits a plain line whose text is the
         message. Non-JSON is therefore the second producer, not a parse failure. On the JSON arm `phase` and
         `reason` are asserted, never defaulted: `reason` is the field naming what to build. */
      let o = null;
      try { o = JSON.parse(ln.slice(5)); } catch (_) { o = null; }
      if (o === null) { extraErrors.push({ context: "why", message: ln.slice(5) }); continue; }
      /* The engine's own line, so input: a record missing `phase` or `reason` is relayed as its raw text. */
      if (typeof o.phase !== "string" || typeof o.reason !== "string")
        extraErrors.push({ context: "why", message: ln.slice(5) });
      else extraErrors.push({ context: o.phase, message: o.reason });
    }
  }
  if (refusal === null && result !== null) {
    const missing = (typeof result !== "object" || Array.isArray(result)) ? ENGINE_DOC_FIELDS
                                                                         : ENGINE_DOC_FIELDS.filter((k) => result[k] === undefined);
    if (missing.length) refusal = "the engine's result document carries no " + missing.join(", ");
    else refusal = resultDocumentRefusal(result, outcome === "complete");
  }
  /* A partial or complete run owes its document; its absence reported as an empty page would be a false clean
     bill. */
  if (refusal === null && result === null && (outcome === "partial" || outcome === "complete"))
    refusal = "the engine session produced no result document";
  if (refusal !== null) {
    result = null;
    extraErrors.push({ context: "result-refused", message: refusal });
    console.warn("[bridge] " + msg.sourceUrl + ": this zone refused the engine's result (" + refusal + ")");
    crashErr = refusal;
    outcome = "crashed";
  }
  /* The instance's routed posts enter the join on every document that carries them, before this one's own
     findings are joined, so a page that posts to itself through a peer is joined on the row it arrives on. */
  if (result && eng) routedPostsTake(eng, msg, result);
  /* …and the identities it emitted join the product, partial or final, so a sibling's next step reads them. */
  if (result && eng) noveltyTake(eng, result);
  /* The absent-document case is handled once on its own arm, so every read on the present arm is off a
     document resultDocumentRefusal accepted, and none is defaulted. */
  /* The counters arm reads `eng._egress`. The two callers that may pass a null `eng` (crashRecord and the
     nothing-to-run arm) never take this branch, so a null here means that split moved. */
  DCHECK(!(outcome !== "crashed" && result) || (eng && eng._egress),
         "a run reached the counters arm with no egress census on its instance — `_egress` is declared in the " +
         "`eng` literal beside `_resolvers`, so an instance without one was built somewhere else, and the row would " +
         "report this zone's own refusals as absent for a run that had them");
  /* The counters arm also reads `eng.residentBytes`, which engineRecordFacts writes before the reservation
     leaves `booting`; only an instance that has stepped reaches here. An absent value would be dropped by the
     structured clone and read as a producer that never existed. */
  DCHECK(!(outcome !== "crashed" && result) || (eng && typeof eng.residentBytes === "number"),
         "a run reached the counters arm with no reported working set on its instance — engineRecordFacts " +
         "states it at the end of every round and before the reservation becomes hot, so its absence is that " +
         "ordering having moved, and the one byte figure on this row that can answer what share of the wasm64 " +
         "address space this instance has taken would be missing from a run that had it");
  const m = (outcome !== "crashed" && result)
    /* The scheduler's own counters, so context switching, forking and preemption are observable. Every value
       is a number off the document asserted above. */
    ? { run: outcome,
        switches: result._switches, flows: result._flows, candidates: result._candidates,
        jobsQueued: result._jobsQueued, jobsRun: result._jobsRun,
        /* The precondition for running a job, beside the job count: reactions that never fire and flows that
           never reach the between-units boundary need opposite fixes (see solver/engine.c's g_units_done). */
        unitsDone: result._unitsDone,
        worldSegmentsHeld: result._worldSegmentsHeld, worldSegmentsMade: result._worldSegmentsMade,
        worldSegmentsForked: result._worldSegmentsForked,
        /* What the work met, which is what tells a page with nothing to find from a page nobody reached. Carried
           on the record because the probe watching this seam cannot read the cumulative store. */
        sourceReads: result._sourceReads, sinkReached: result._sinkReached,
        sinkTainted: result._sinkTainted, sinkNeedsSender: result._sinkNeedsSender,
        /* Uncalled-code driving: `driven` alone cannot tell a page that ships no uncalled code from a scheduler
           that never reached the question; `asked` can. */
        orphansDriven: result._orphansDriven, orphansAsked: result._orphansAsked,
        /* Which exit each ask took. `memo` is an ask the generation cache answered with no walk; `empty` a walk
           that found nothing (a fact about the heap: the walk sees only bodies with a live function object);
           `took` a productive walk. A low `memo` points at walk cost, which `orphanWalks` and its siblings
           price. The three sum to `asked`, which solver/result.c asserts. */
        orphanAskMemo: result._orphanAskMemo, orphanAskEmpty: result._orphanAskEmpty,
        orphanAskTook: result._orphanAskTook,
        /* Walk cost with both denominators. `entries / walks` is the mean object-list length one take reads;
           `fullCandidates / walksFull` is the mean candidate population over walks that reached the end of the
           list, since an early exit on a preferred candidate sees only a floor of the set. `walksFull` 0 with
           `walks` nonzero means every take was decided early. solver/result.c asserts
           `walks == empty + took`. */
        orphanWalks: result._orphanWalks, orphanWalkEntries: result._orphanWalkEntries,
        orphanWalksFull: result._orphanWalksFull,
        orphanWalkFullCandidates: result._orphanWalkFullCandidates,
        /* What the run learned (`endpoints`, `sinks` below), beside what it cost. The crash arm carries
           neither: a zero there would read as an analysed, clean page. */
        /* The WFQ census at the instant the document was composed. It is a reading of an instant, not a
           total, so it stays one nested object. It crosses whole: this zone reads no row of it, so a row added
           to the census reaches the popup unedited. A partial's census is the informative one (qjs_emit_partial
           composes one every PARTIAL_MS over a live frontier); a finalize's is composed after drain or park and
           reads `{members: 0}`. */
        wfq: result._wfq,
        /* The subsystem censuses, crossing whole for the same reason. They stay separate objects because a
           reader compares within a census, never across. */
        cold: result._cold, heap: result._heap, swap: result._swap, forkAt: result._forkAt,
        /* Standard-owned names the document read and this realm did not answer, with the population of global
           reads they are drawn from. */
        absent: result._absent,
        /* What the order above was denominated in, relayed whole. It is a property of the host and build,
           constant for the session. With no `-pthread` in the link, linear memory is an ordinary ArrayBuffer
           and no second thread can raise the yield request, so the aging charge is billed in wall time and the
           OS's descheduling decides part of the order; two runs of one build can differ, and the popup renders
           this beside the order so that is not read as an engine change. solver/quantum.c's #error keeps the
           link and this denomination in step. */
        quantum: result._quantum,
        /* What this zone's egress policy did, which the engine's counters cannot say: a page with no API
           surface is either one the engine never derived requests for or one whose requests this tool refused,
           and the two need opposite work (improve driving, or widen the origin).
           `egressAsked` is the denominator; `egressDeclined` is keyed on the rule, not the signal name, since
           rules differ in whether a person can reopen them. It is a different denominator from the engine's
           `cold.replyDeclined` (one decline of a (method, url) pair marks every parked record on it); this row
           nonzero with `replyDeclined` 0 is a refusal that never reached the engine.
           The map is copied so the row is one moment: a live reference would keep changing after the partial
           was composed, out of step with `egressAsked`. Null-prototype in (exact `in`), plain object out, like
           every other census on the row. */
        egressAsked: eng._egress.asked, egressDeclined: Object.assign({}, eng._egress.declined),
        /* The instance's whole linear memory, `M.HEAPU8.length` as renderer.html stated it on the reply
           engineRecordFacts last awaited: arena plus stack plus static data, so it is the only figure here that
           answers what share of the wasm64 address space the instance has taken (`heap.arenaKiB` is a subset).
           It is separate from `heap`, whose rows build.mjs derives from result_heap_json's format string, and
           it is a different observer at a different moment, so no quotient against `heap` is composed. A wasm
           Memory never shrinks, so the latest value is the high-water mark and a share of the per-module ceiling
           is sound; `_residentBytes` sums it across engines against a device RAM floor, a different question.
           level1AccountRefusal refuses any other shape where the value enters this zone. */
        workingSetBytes: eng.residentBytes,
        endpoints: result.fetchCallSites.length, sinks: result.securitySinks.length,
        /* What composed each address (`door`) and whether the page's own code had run when it did
           (`mintedAt`), per row, so the share of the surface owed to forced execution is read off the rows
           rather than by subtracting two totals. Two histograms, not one keyed on the pair: solver/endpoint.c
           states they are two facts, and a reader reads the margins. Each sums to `endpoints`. */
        endpointDoors: endpointFactHistogram(result.fetchCallSites, "door"),
        endpointMintedAt: endpointFactHistogram(result.fetchCallSites, "mintedAt"),
        /* The address class, the one of these that answers the product's bar (an address no parse of the served
           bytes can state). Doors cannot be coarsened into it: a literal chunk URL via `module-import` is beyond a
           markup parse yet clears nothing, while `/api/{location.hash}` via `fetch` clears it.
           It is a floor, not a verdict: `unknown` means the run reached the address holding a value it had not
           determined and resting on an unknown supplied from outside the engine, so those rows clear the bar;
           `concrete` claims nothing about a parse (solver/endpoint.h enumerates what it hides). This zone holds
           no copy of that classification, so a class added to `ENDPOINT_ADDRESS_CLASSES` reaches it unedited, and
           a row from an older wasm lands in `(unstated)`. */
        endpointAddressClass: endpointFactHistogram(result.fetchCallSites, "addressClass"),
        /* The razor class, the figure a person may read as the bar: solver/endpoint.c unions the address
           class's `unknown` rows with the rows whose door handed it bytes never in the served document, per row,
           where both facts are in hand. The union is composed there, not here, so this zone holds no door map
           that would abort it when the engine adds a door (and `EPR_BEYOND`, the markup-parse class, is not the
           operand). It derives from `door` and `addressClass` and adds no observation: `unproven` is not a claim
           that a parse could state the address. Rendered and asserted generically, like the rows above. */
        endpointRazorClass: endpointFactHistogram(result.fetchCallSites, "razorClass"),
        /* The witness class, sizing the population `unproven` hides (a source a flow pinned and re-read). It is
           never unioned into the bar: `may-rest-on` is a fact about the path, not this address's bytes, so
           folding it in would turn a floor into an over-claim. The columns are read side by side. Rendered
           and asserted generically. */
        endpointWitnessClass: endpointFactHistogram(result.fetchCallSites, "witnessClass"),
        /* The delivery-root state the address class derives from, sizing the population whose `unknown` is a
           hole `engine_orphan_call` minted for its own drive (solver/endpoint.c's `address_class_of` residual).
           It is a third observation about the same array and is not unioned into or subtracted from the others:
           `(named)` does not claim the bytes are the page's, nor `(unattributed)` that they are the engine's.
           The names stay on the emitted row, since a page-influenced string may not be a key here. */
        endpointAddressRoot: endpointAddressRootHistogram(result.fetchCallSites),
        park: result._park.length, resumed: resumed,
        coldLookup: cold.lookup, coldOther: cold.other, bundleId: cold.bundle,
        /* This document's findings over a routed message, joined to the sender's post (routedPostJoins). */
        routedJoins: routedPostJoins(msg, result),
        url: (msg && msg.sourceUrl) || "" }
    /* A crashed run reports no cost counters: zeroes would read as a run that explored nothing. `run` keeps the
       three states (partial, ended, died) apart. The arm is chosen by the outcome, not by whether a document
       arrived, so a crash that left an unconsumed partial logs as crashed; its findings still travel on the
       returned analysis labelled `_run:"crashed"`. */
    /* It does carry its cause, `err`, the same root @WHY string engineCrash puts on the banner, so the row
       the diagnostics page's GET_ENGINE_RUNS and testing/live-run.js read names the capability that failed. */
    : { run: outcome, resumed: resumed,
        coldLookup: cold.lookup, coldOther: cold.other, bundleId: cold.bundle,
        url: (msg && msg.sourceUrl) || "", err: crashErr };
  /* Both crash producers write the `engine-crash` line before calling here, so an empty `err` (asserted below)
     is a third crash path or a producer that stopped writing it. */
  /* The egress containment: `asked` and each decline are raised in engineServiceFetch's one loop (one ask per
     iteration, at most one decline in it), so a histogram summing past its denominator is a second raiser. */
  if (outcome !== "crashed" && result) {
    /* Read off the row's copy, not `eng`, so it is an assertion about what the row states. */
    let _sum = 0;
    for (const k of Object.keys(m.egressDeclined)) _sum += m.egressDeclined[k];
    DCHECK(_sum <= m.egressAsked,
           "this zone's egress census counted " + _sum + " refusal(s) against " + m.egressAsked + " " +
           "request(s) asked of it — the two are raised in one loop, one `asked` per delivered pending line " +
           "and at most one refusal inside that same iteration, so a sum above the denominator is a second " +
           "site raising one of them and every share read off this pair is over a population that never ran");
    /* The endpoint partitions are partitions: each histogram sums to `endpoints`, which fails if one is built
       over a filtered or deduped walk. (A key stated on some rows only is refused at the door.) This list is the
       only statement of which partitions are checked, so a histogram added above needs a line here. */
    for (const _p of [["endpointDoors", m.endpointDoors], ["endpointMintedAt", m.endpointMintedAt],
                      ["endpointAddressClass", m.endpointAddressClass],
                      ["endpointRazorClass", m.endpointRazorClass],
                      ["endpointWitnessClass", m.endpointWitnessClass],
                      ["endpointAddressRoot", m.endpointAddressRoot]]) {
      let _n = 0;
      for (const k of Object.keys(_p[1])) _n += _p[1][k];
      DCHECK(_n === m.endpoints,
             "this run's `" + _p[0] + "` partition sums to " + _n + " over an emitted surface of " +
             m.endpoints + " row(s) — both are read off the ONE `fetchCallSites` array at this composition, " +
             "so a difference is a walk that filtered or deduped rows the figure beside it still counts, and " +
             "the partition would be read as a statement about the surface a person is shown");
    }
  }
  DCHECK(outcome !== "crashed" || (typeof crashErr === "string" && crashErr !== ""),
         "a crashed run reached the run log with no `engine-crash` line among its output — every crash path " +
         "writes one (engineCrash appends the ROOT @WHY to it, crashRecord constructs it), so a crash with " +
         "no cause here is a producer that stopped announcing itself and a row a reader cannot act on");
  // A per-run log, not a single overwritten global: concurrent engines each report here, so the whole
  // park, persist, rehydrate, resume sequence across engines is observable.
  /* A document with nothing to run produces no record, because no engine ran: an absent row, a row of zeroes
     and a crash row are three different facts. */
  if (outcome !== "nothing-to-run") engineLogWrite(eng, m);
  /* The engine-owned fields `fetchCallSites`, `securitySinks` and `_park` are present or absent, never empty
     substitutes: their presence states that an @RESULT document arrived and was asserted, and every consumer
     reads that presence (lib/merge.js, offscreen-brain's merges, the frontier write). An empty substitute would
     make "no document arrived" and "analysed and clean" identical. `resolverErrors` is the host's record and is
     on both arms; on the no-document arm it carries the `@E engine-crash` row. `_run` says which absence.
     The object carries only what the offscreen reads; a constant `[]` is not a measurement and is not shipped. */
  const analysis = {
    /* The engine's page errors as `{context, message}` rows of two strings. `context` is a closed 2x2: the
       base is `page` (standing) or `page-retracted` (taken back, HTML §8.1.6.4
       HostPromiseRejectionTracker(promise, operation), still worth showing because it names a capability the
       page reached for), and `-explored` is appended where the throw was engine-minted exploration, since
       `pageErrorsExplored` overlaps the other two and concatenating it would show one message twice. popup.js
       renders `context + ": " + message` verbatim, so nothing downstream learns a new shape. A Set keeps the
       lookup linear. */
    resolverErrors: (function () {
      if (!result) return [].concat(extraErrors);
      const explored = new Set(result.pageErrorsExplored.map((e) => String(e)));
      const row = (base) => (e) => {
        const message = String(e);
        return { context: explored.has(message) ? base + "-explored" : base, message };
      };
      return result.pageErrors.map(row("page"))
          .concat(result.pageErrorsRetracted.map(row("page-retracted")))
          .concat(extraErrors);
    })(),
    /* No probeResults on this seam: the engine issues no requests. lib/req2proto.js (driven by
       lib/discovery-probe.js and lib/response-decode.js) writes `globalStore.probeResults` directly. */
    // The document this analysis is about, asserted at the top of this function rather than defaulted here.
    sourceUrl: msg.sourceUrl,
    /* What this analysis is a record of (see `outcome` above); written on every arm, so consumers assert it. */
    _run: outcome,
  };
  /* The engine-owned fields, added only where the engine stated them, read straight off `result` so no object
     here exists for a document to be defaulted into. */
  if (result) {
    analysis.fetchCallSites = result.fetchCallSites;
    analysis.securitySinks = result.securitySinks;
    analysis._park = result._park;
    analysis._parkWhy = result._parkWhy;
    /* The run's novelty record, which the finish checks its parked row's value against (`landed`, `paid`). */
    analysis._novelty = result._novelty;
  }
  return analysis;
}
/* The engine-document fields every consumer reads, refused at linesToAnalysis' door when any is absent. */
const ENGINE_DOC_FIELDS = ["fetchCallSites", "securitySinks", "_park", "_parkWhy", "_novelty"];
/* Whether an analysis carries an engine document, asked in one place so the five fields cannot drift apart
   across consumers. True means an @RESULT arrived and was accepted; false means none did and `_run` says which
   absence. linesToAnalysis copies all five or none and refuses a document missing one, so a record with some is
   that composition broken, which is this zone's own code. */
function analysisHasDocument(a) {
  DCHECK(a && typeof a === "object", "an analysis record is not an object — every producer of one is in this file");
  const n = ENGINE_DOC_FIELDS.filter((k) => a[k] !== undefined).length;
  DCHECK(n === 0 || n === 5,
         "an analysis carries " + n + " of the five engine-document fields (fetchCallSites, securitySinks, " +
         "_park, _parkWhy, _novelty) — linesToAnalysis writes all five or none, because their presence IS " +
         "the statement that an @RESULT document arrived, and a partial set would be read by every consumer " +
         "as a document that is there while one of the surfaces it travels in is silently gone");
  DCHECK(n === 5 || a._run === "crashed" || a._run === "nothing-to-run",
         "an analysis marked `" + a._run + "` carries no engine document — only a crashed instance and a page " +
         "with nothing to run may lack one, so a partial/complete run without one is the @RESULT relay broken " +
         "and reporting it as a page with no API surface is a false clean bill");
  return n === 5;
}
self.analysisHasDocument = analysisHasDocument;   // read by offscreen-brain.js and lib/merge.js across the zone's one realm

/* The origin of a URL, or "" when it does not parse. */
function originOf(u) { try { return new URL(u).origin; } catch (_) { return ""; } }
/* The origin stamped on a delivered message, serialized as HTML does. SECURITY.md: routing and the origin
   stamped on a delivered message belong to the trusted zone alone, since a forgeable `event.origin` defeats
   every origin check in the analysed bundle. This zone holds `_senderOrigin`'s per-document "null:<uuid>"
   token for an opaque document, so two opaque documents never compare same-origin; that token is internal,
   and HTML §7.1.1 Origins serializes an opaque origin as "null", which is what pages compare against. */
function stampOrigin(o) { return _isRealOrigin(o) ? o : "null"; }
/* The agent cluster an instance is, SECURITY.md's (browsing-context group, origin), computed here because
   both halves are browser-stated and the engine may state neither. Same-origin documents of one group share
   one similar-origin window agent and therefore one heap (navigable.c's `child_in_this_agent` builds a
   same-origin child as a second realm in one JSRuntime); one instance still holds exactly one origin, so a
   fetch out of it has one principal.
   The group half is `sender.tab.id`: no API exposes a browsing-context group, and a tab never joins documents
   from different groups. Named residual: an auxiliary context opened without `noopener` shares its opener's
   group but lands in another tab; next diff joins it to its opener's group from a browser-stated opener fact;
   absence shows as an opener and its popup in two instances. Engine-opened auxiliaries take their creator's group.
   The origin half is `_senderOrigin`'s, opaque-unique, never `originOf(sourceUrl)` (a sandboxed document's
   address parses to a tuple origin the browser refused it). A rehydrated cold recipe's group is `cold:` plus
   its frontier key (`<address>|<bundle>`), unique per recipe, so its stored principal, even an empty one,
   collides with nothing. A NUL separator, because neither half can contain one. */
function clusterKeyOf(msg) {
  DCHECK(msg && msg.groupId != null && msg.groupId !== "",
         "a document reached the pool naming no browsing-context group — an instance IS its (group, origin) " +
         "agent cluster, so a document with no group would share one cluster (and one heap, and one principal) " +
         "with every document of its origin in every tab the user has open");
  DCHECK(typeof msg.origin === "string",
         "a document reached the pool with origin " + String(msg.origin) + " — the origin half is the " +
         "principal `_senderOrigin` or the record stated, a string even when empty (a cold recipe's), and a " +
         "missing one would key with every empty principal of its group");
  return String(msg.groupId) + "\u0000" + msg.origin;
}
/* The frontier key is the document's address, identity and program: `address|identity|bundle` (frontierKeyOf,
   whose identity half separates partitions and credential states), with the bundle id
   computed by the engine (qjs_bundle_id, a Lexbor <script> scan of the external src set, whose content-hashed
   names are the app version). A recipe is a path, not a function: solver/cold.c writes `f<chain-id>,<reward>`,
   a positional id into the decision chain that document's own scripts built, so it replays only in the same
   document running the same program. The address (DOM §4.5 Interface Document) separates one SPA route from
   another, since an SPA ships one bundle to every route; the bundle half invalidates a residue when a redeploy
   changes a src. A document at a volatile address keys a new entry per visit and never resumes, which is
   correct: no one will visit that address again. */
/* The cross-session flow frontier (IndexedDB). The learned surface persists in globalStore; this persists the
   unfinished frontier as compact replay recipes, so a parked frontier resumes on a later visit or session as
   one continuous attention across sessions. */
/* The store version is the entry grammar's version, executed in the upgrade transaction (IndexedDB §5.7
   Upgrading a database). Each arm states the version that introduced it and runs only for older profiles,
   so a future version bump never wipes the frontier as a side effect.
   v2 added `responseHeaders` (see frontierDoc): a v1 entry cannot state the response its document came from,
   so it cannot resume under the right policy container and is deleted rather than defaulted to no CSP.
   v3 adds `prefs` (the configured device share, see residency, and the egress table): this zone's state lives
   in IndexedDB, never `chrome.storage.local`, beside the store it is about.
   v4 adds `novelty`, lib/novelty-set.js's partitions keyed by origin: the identities every session of a
   cluster emitted, which is what makes a resumed or second document's reward novelty against all of them.
   v5 adds `bodies` (lib/body-store.js): an entry names its document by digest (`doc`) and the bytes are stored
   once, counted by the entries naming them. Entries an older build wrote inline are converted once, after the
   open, by frontierBodiesReady, since a digest cannot be computed inside the upgrade transaction.
   v6 replaced an entry's `emit`/`visits` with a Level-1 account, and v7 makes it `value`, `pulls` and `at`
   (see level1Index): counts held at a product size. An older account is in another currency, so an entry
   carries its recipes forward and restarts unserved. */
function idbOpen() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("apiclient-frontier", 8);
    r.onupgradeneeded = (ev) => {
      const db = r.result;
      if (ev.oldVersion < 2) {   // v1's entry grammar cannot state its document's response; it is not resumable
        if (db.objectStoreNames.contains("frontier")) db.deleteObjectStore("frontier");
        db.createObjectStore("frontier");
      }
      if (ev.oldVersion < 3 && !db.objectStoreNames.contains("prefs")) db.createObjectStore("prefs");
      if (ev.oldVersion < 4 && !db.objectStoreNames.contains("novelty")) db.createObjectStore("novelty");
      if (ev.oldVersion < 5 && !db.objectStoreNames.contains("bodies")) db.createObjectStore("bodies");
      // v8 adds `httpcache`, the HTTP cache's records (httpCacheWrite); its bodies are `bodies`'.
      if (ev.oldVersion < 8 && !db.objectStoreNames.contains("httpcache")) db.createObjectStore("httpcache");
      if (ev.oldVersion >= 2 && ev.oldVersion < 7) {
        const c = r.transaction.objectStore("frontier").openCursor();
        c.onsuccess = () => {
          const cur = c.result;
          if (!cur) return;
          const e = cur.value;
          delete e.emit; delete e.visits; delete e.paid; delete e.serviceUs;
          e.value = LEVEL1_UNSERVED.value; e.pulls = LEVEL1_UNSERVED.pulls; e.at = LEVEL1_UNSERVED.at;
          cur.update(e);
          cur.continue();
        };
      }
    };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
/* The preference edge. Failures are surfaced like the frontier's own (frontierFail), so an unreadable
   preference is not mistaken for one the user never set. */
function frontierPref(name) {
  return new Promise((res, rej) => {
    idbOpen().then((db) => {
      const t = db.transaction("prefs").objectStore("prefs").get(name);
      t.onsuccess = () => res(t.result === undefined ? null : t.result);
      t.onerror = () => rej(t.error);
    }, rej);
  }).catch((e) => { RETHROW_FATAL(e); frontierFail("preference read", e); return null; });
}
/* One readwrite transaction over `store`, whose requests `write(objectStore)` issues, settled by the
   transaction: resolved on `complete` and rejected on `abort`. A request's `success` is not a commit: IndexedDB
   3 §5.4 "Committing a transaction" writes the changes after every request is processed, and "Only after the
   transaction has been successfully written is the complete event fired"; a failed write, or anything else,
   aborts it (§5.5 "Aborting a transaction"), which reverts every change. `durability` is §2.7
   "Transactions"' hint. */
function idbWrite(store, durability, write) {
  return idbOpen().then((db) => new Promise((res, rej) => {
    const tx = db.transaction(store, "readwrite", { durability });
    tx.oncomplete = () => res();
    tx.onabort = () => rej(tx.error || new DOMException("the " + store + " transaction was aborted", "AbortError"));
    write(tx.objectStore(store));
  }));
}
/* A person's setting, written "strict": §2.7 asks for "strict" "in cases where reducing the risk of data loss
   outweighs the impact to performance and power", and a setting is written once per act. Rejects with the
   transaction's failure, which the caller reports (prefWriteOutcome). */
/* idbWrite over several stores in one transaction, so an entry and the body references it implies commit
   together; `write` receives the object stores by name. */
function idbWriteStores(stores, durability, write) {
  return idbOpen().then((db) => new Promise((res, rej) => {
    const tx = db.transaction(stores, "readwrite", { durability });
    tx.oncomplete = () => res();
    tx.onabort = () => rej(tx.error || new DOMException("the " + stores.join("+") + " transaction was aborted",
                                                        "AbortError"));
    const by = {};
    for (const n of stores) by[n] = tx.objectStore(n);
    write(by);
  }));
}
function frontierPrefPut(name, value) {
  return idbWrite("prefs", "strict", (s) => s.put(value, name));
}
/* A setting write's outcome for the reply that shows it: null once committed, or the failure. The setting is
   in force for this session either way; the reply says whether the next one will have it. */
async function prefWriteOutcome(write) {
  try { await write; return null; }
  catch (e) { RETHROW_FATAL(e); return String((e && e.name) || "Error") + ": " + String((e && e.message) || e); }
}
/* ─── The product's known identities, persisted (lib/novelty-set.js holds the set) ─────────────────────
   Restored once, before the first instance is handed its partition, so a session's first engine is judged
   against every earlier session; written back whenever a result grows a partition. The write is "relaxed":
   a lost write costs a later session one point per identity it re-finds, and the next result rewrites it. */
let _noveltyRestored = null;
function noveltyReady() {
  if (_noveltyRestored === null) _noveltyRestored = (async () => {
    try {
      const db = await idbOpen();
      const recs = await new Promise((res, rej) => {
        const t = db.transaction("novelty").objectStore("novelty").getAll();
        t.onsuccess = () => res(t.result); t.onerror = () => rej(t.error);
      });
      for (const rec of recs) noveltyRestore(rec);
    } catch (e) { RETHROW_FATAL(e); frontierFail("novelty read", e); }
  })();
  return _noveltyRestored;
}
let _noveltySaveTimer = null;
function noveltyPersistSoon() {
  if (_noveltySaveTimer !== null) return;
  _noveltySaveTimer = setTimeout(() => {
    _noveltySaveTimer = null;
    const recs = noveltyTakeDirty();
    idbWrite("novelty", "relaxed", (s) => { for (const rec of recs) s.put(rec, rec.partition); })
      .catch((e) => { RETHROW_FATAL(e); frontierFail("novelty write", e); });
  }, 2000);
}
/* One result document's identities into the emitting instance's partition: the origin this zone stamped on
   its cluster (lib/novelty-set.js says why it is never the engine's word). */
function noveltyTake(eng, result) {
  const r = noveltyAbsorb(eng.origin, result._novelty, eng._noveltyCounted);
  for (const x of r.refused)
    console.warn("[bridge] " + eng.msg.sourceUrl + ": a novelty line was refused (" + x.reason + "): " + x.line);
  if (r.added > 0) noveltyPersistSoon();
}
/* What `eng` is owed of its partition, handed now; called at its boot before Init and before each step, so
   it is serialized with the instance's other calls. */
async function noveltyHand(eng) {
  const text = noveltyOwed(eng.origin, eng._novelty, eng._noveltyCounted);
  if (text !== null) await eng.r.renderer.noveltyLearn({ text });
}
/* ─── The per-origin exploration widening, persisted ──────────────────────────────────────────────────
   The decision lives in lib/safe-fetch.js (the table, what a widening means, the refusal). This file holds
   only what is the host's: where a person's standing permission is kept between sessions, and when it is
   stated; engine/trusted.mjs answers both differently for its command line. It sits in the `prefs` store as
   a record of what this person permitted, outside the cumulative store, so a store-shape migration can
   never drop or invent a permission.
   The store follows the table: every grant and revocation goes into the chokepoint first and is written back
   from `safeFetchEgressTable()`, so the persisted copy is a projection, never a second registry.
   `null` means the restore has not been started; the chokepoint itself keeps "stated empty" and "not yet
   stated" apart. */
let _egressPolicyStated = null;
/* The origins whose standing permissions this restore could not carry forward, kept as a list so the surface
   can name them; a permission that silently vanishes reads as a broken control or as still granted. */
let _egressLegacyDropped = [];
function egressPolicyReady() {
  if (_egressPolicyStated === null) _egressPolicyStated = (async () => {
    const stored = await frontierPref("exploreOrigins");
    /* The stored value is this zone's own write, so an unreadable shape is corruption, except an array: the
       previous single-switch control's legacy shape. It is dropped rather than carried forward, because a
       bare origin meant "fire everything here" and everything now includes signals that did not exist then;
       the person re-permits per signal and is told. `safeFetchEgressStated` re-asks each member with the
       chokepoint's own predicate and returns the entries it could not use. */
    DCHECK(stored === null || typeof stored === "object",
           "the stored per-origin egress table is neither null nor an object (" + String(stored) + ") — this " +
           "zone is its only writer, so this is that record corrupted, and reading past it would state an " +
           "EMPTY table while every origin this person has permitted silently stopped being permitted");
    if (Array.isArray(stored)) {
      /* The legacy shape is translated here: the chokepoint refuses an array outright (a CHECK). */
      _egressLegacyDropped = stored.filter((o) => typeof o === "string");
      self.safeFetchEgressStated({});
    } else {
      _egressLegacyDropped = self.safeFetchEgressStated(stored === null ? {} : stored);
    }
  })();
  return _egressPolicyStated;
}
/* The restore is started at this zone's load and its promise is handed to the chokepoint, which awaits it in
   `safeFetch`, so every asker waits for the person's table. Two entries reach the network: the engine behind
   astDispatch, and passive learning (handleResponseBody's discovery sweep), which runs from intercept.js at
   `document_start`, earlier than content.js's `document_idle` message; registering at the chokepoint covers
   both by construction. */
DCHECK(typeof self.safeFetchEgressStating === "function",
       "this zone has no `safeFetchEgressStating` to hand its egress-policy read to — ast-worker.html loads " +
       "lib/safe-fetch.js ahead of this file precisely so that the chokepoint exists before the host that " +
       "feeds it, and without the registration every request would race the restore and be refused with a " +
       "row of the person's own control they never ticked");
self.safeFetchEgressStating(egressPolicyReady());
/* ─── The HTTP cache's store (lib/http-cache.js holds the cache) ───────────────────────────────────────
   `httpcache` holds one record per partition and target URI, {key, partition, target, entries}, each entry
   naming its body by digest in `bodies` (lib/body-store.js), so a reply a frontier entry also holds is stored
   once. Restored before the first selection (the chokepoint awaits it), and written back "relaxed" a moment
   after a change: a lost write costs one refetch. `_httpCacheDisk` maps each stored record's key to its
   bodies' digests and bytes, which residency counts against the share. */
const _httpCacheDisk = new Map();
function httpCacheDiskRow(stored) {
  return new Map(stored.entries.map((e) => [e.body, e.bodyBytes]));
}
async function httpCacheReadStore() {
  try {
    const db = await idbOpen();
    const recs = await new Promise((res, rej) => {
      const tx = db.transaction(["httpcache", "bodies"]);
      const out = [];
      tx.oncomplete = () => res(out);
      tx.onabort = () => rej(tx.error);
      const all = tx.objectStore("httpcache").getAll();
      all.onsuccess = () => {
        for (const rec of all.result) {
          const entries = rec.entries.map((e) => Object.assign({}, e));
          out.push({ rec, entries });
          for (const e of entries) {
            const g = tx.objectStore("bodies").get(e.body);
            g.onsuccess = () => {
              DCHECK(g.result !== undefined, "a stored HTTP cache entry names a body the store does not hold — " +
                     "the record and its reference are written in one transaction, so the two have parted");
              e.body = g.result === undefined ? null : g.result.value;
            };
          }
        }
      };
    });
    for (const { rec, entries } of recs) {
      /* The release arm under the DCHECK above: a record missing a body answers nothing and is deleted. */
      if (entries.some((e) => e.body === null)) {
        await httpCacheWrite({ key: rec.key, partition: null, target: null, entries: [] });
        continue;
      }
      _httpCacheDisk.set(rec.key, httpCacheDiskRow(rec));
      httpCacheRestore({ partition: rec.partition, target: rec.target, entries });
    }
  } catch (e) { RETHROW_FATAL(e); frontierFail("http cache read", e); }
}
/* One changed record (httpCacheTakeDirty's) into the store: its bodies referenced by digest, the old record's
   unreferenced, in one transaction. A distinct digest counts once per record, so a record naming one body
   twice holds one reference; an empty record is deleted. */
async function httpCacheWrite(rec) {
  const stored = { key: rec.key, partition: rec.partition, target: rec.target, entries: [] };
  const values = new Map();
  for (const e of rec.entries) {
    const digest = await bodyDigest(e.body);
    values.set(digest, e.body);
    stored.entries.push(Object.assign({}, e, { body: digest, bodyBytes: bodyBytes(e.body) }));
  }
  await idbWriteStores(["httpcache", "bodies"], "relaxed", (st) => {
    const g = st.httpcache.get(rec.key);
    g.onsuccess = () => {
      const old = new Set(g.result ? g.result.entries.map((e) => e.body) : []);
      for (const d of old) if (!values.has(d)) bodyUnref(st.bodies, d);
      for (const [d, v] of values) if (!old.has(d)) bodyRef(st.bodies, d, v);
      if (stored.entries.length > 0) st.httpcache.put(stored, rec.key); else st.httpcache.delete(rec.key);
    };
  });
  if (stored.entries.length > 0) _httpCacheDisk.set(rec.key, httpCacheDiskRow(stored));
  else _httpCacheDisk.delete(rec.key);
}
let _httpCacheSaveTimer = null;
function httpCachePersistSoon() {
  if (_httpCacheSaveTimer !== null) return;
  _httpCacheSaveTimer = setTimeout(async () => {
    _httpCacheSaveTimer = null;
    try {
      for (const rec of httpCacheTakeDirty()) await httpCacheWrite(rec);
      await frontierResidency();
    } catch (e) { RETHROW_FATAL(e); frontierFail("http cache write", e); }
  }, 2000);
}
/* Every stored record off the disk (the in-memory cache keeps answering this session), for residency. */
async function httpCacheShed() {
  for (const [key, row] of [..._httpCacheDisk]) {
    DCHECK(row.size > 0, "a stored HTTP cache record names no body — an empty record is deleted, not kept");
    await httpCacheWrite({ key, partition: null, target: null, entries: [] });
  }
}
self.httpCacheStated(httpCacheReadStore(), httpCachePersistSoon);
/* This extension's own third-party-cookie setting (lib/request-identity.js), kept in the `prefs` store beside
   the egress table and restored once at load; a send that needs it before the restore lands awaits it. Only
   the AST_THIRD_PARTY_COOKIES arm below changes it, and only for the popup. */
const _thirdPartyCookiesReady = (async () => {
  self.identityThirdPartyCookiesStated(self.identityThirdPartyCookiesRestored(await frontierPref("thirdPartyCookies")));
})();
self.identityThirdPartyCookiesStating(_thirdPartyCookiesReady);
/* The write-back, taken off the table rather than off the message that changed it, so a grant the chokepoint
   refused is never persisted. */
function egressPolicyPersist() {
  /* The whole table, not an origin list: a permission is per signal and per value. `safeFetchEgressTable`
     answers in exactly the shape `safeFetchEgressStated` takes. */
  return frontierPrefPut("exploreOrigins", self.safeFetchEgressTable());
}
/* A frontier entry: { key: address|identity|bundle, sourceUrl, topLevelUrl, origin, responseHeaders, html, code,
   recipes, value, pulls, at, credentialed, provenance }, plus `shed`/`stranded` for a shed entry and `crash`
   for one whose resume aborted the engine (frontierRecordCrash), and `rest` for one written because every member
   rests on a refused request (`_parkWhy` "refused"). Rehydration
   rebuilds the document from (html, code) and resumes the recipes, so a parked flow on any site can advance
   later even when that page is not open. */
/* The document half of a cold-tier entry, one speller for the store's writer and its reader. Recipes replay
   inside a document, so the entry carries exactly what `qjs_init` takes: its bytes, its URL (DOM §4.5
   Interface Document), the top-level creation URL (HTML §8.1.3.1 Environments), its browser-stated principal,
   and the response header list that HTML §7.1.7 Policy containers makes its policy container (CSP, COOP,
   COEP, and `Origin-Agent-Cluster`). None is derivable from the rest. Nothing is defaulted: `{}` is the header
   list of a response with no headers, and substituting it would turn a CSP-protected page into an unprotected
   one and report exploits that are not real. */
/* The half of an entry that holds whether or not it still has its document: the address the recipes replay
   at, the environment, the principal and the policy. A shed entry is asserted by the same sentences. */
function frontierPlace(e, when) {
  DCHECK(e && typeof e === "object",
         "a cold-tier entry " + when + " as no record at all — the entry IS the parked document, so there is " +
         "nothing to resume the recipe's flows inside of");
  DCHECK(typeof e.sourceUrl === "string" && e.sourceUrl !== "",
         "a cold-tier entry " + when + " with no document URL — DOM §4.5 \"Interface Document\" gives every " +
         "Document one, the engine derives this document's principal from it, and every relative URL the " +
         "bundle builds resolves against it");
  DCHECK(typeof e.topLevelUrl === "string" && e.topLevelUrl !== "",
         "a cold-tier entry " + when + " with no top-level creation URL — HTML §8.1.3.1 \"Environments\" " +
         "defines it and §8.1.3.5 \"Secure contexts\" reads it, so the flows would resume against a different " +
         "set of Web IDL §3.3.13 \"[SecureContext]\" members than they parked against; the engine refuses an " +
         "empty one one call later, in the process handed the hole rather than in the zone that made it");
  DCHECK(typeof e.origin === "string",
         "a cold-tier entry " + when + " with no principal — it is browser-stated, this zone cannot re-derive " +
         "it from the address (a sandboxed document's address lies about it), and a resumed instance that " +
         "posts a cross-document message must stamp the origin the parked one had");
  DCHECK(e.responseHeaders && typeof e.responseHeaders === "object",
         "a cold-tier entry " + when + " with no response header list — HTML §7.1.7 \"Policy containers\" " +
         "makes those headers this Document's policy container, so the parked flows would resume under a " +
         "policy nobody delivered and every @S verdict on them would be decided against it");
  /* How the load that stored it got its address: absent (an entry written before the field existed, see
     frontierProvenance) or one of the three words; a fourth would reach `safeFetch` as an invented grade. */
  DCHECK(e.provenance === undefined || e.provenance === PROVENANCE_OBSERVED ||
         e.provenance === PROVENANCE_DERIVED || e.provenance === PROVENANCE_FORCED,
         "a cold-tier entry " + when + " stating the provenance `" + String(e.provenance) + "`, which is none " +
         "of the three — the re-derivation of a shed entry is decided from this word, and one the chokepoint " +
         "does not know would abort the fetch rather than be refused by the policy it names");
  return e;
}
/* The provenance of the load that stored this entry, for the re-fetch that brings its document back.
   An absent field is a positive statement: every entry a live build parks writes it (asserted at frontierPut),
   so a missing one was written by a build with no door onto `safeFetchWiden`, where the chokepoint refused
   every forced load and every entry came from an ambient navigation (`observed`) or a declared route
   (`derived`). The weaker, `derived`, is answered: under-claiming is the allowed direction (solver/engine.h's
   `engine_provenance_of_running_path`), and its only cost is the destructive-path deny list staying armed.
   The grade is never read off the address, since that would let a widening for one origin apply to another. */
function frontierProvenance(e) {
  return e.provenance === undefined ? PROVENANCE_DERIVED : e.provenance;
}
/* The record a request or notice from instance `eng` is judged against, given the documents the engine names
   in `role` (lib/document-record.js's docRecordAskerRefusal): the meet of their records (docRecordOfAsker,
   docRecordMeet) as `{doc, refusal: null}`, or `{doc: null, refusal}` naming the first that is not a document
   of `eng`, which refuses it. A name another live instance holds is that instance's; one no instance holds is
   walked to its creator. */
function askerRecord(eng, names, role) {
  const lookup = (n) => {
    if (eng.records.has(n)) return eng.records.get(n);
    const h = hostHolderOf(n);
    CHECK(h !== eng, "instance holds the document " + n + " and has no record of it — engineRoot and " +
                     "engineJoin write the record beside the name");
    return h === null ? undefined : false;
  };
  const recs = [];
  for (const n of names) {
    const r = docRecordOfAsker(n, lookup);
    if (r.refusal) return { doc: null, refusal: docRecordAskerRefusal(role, r.refusal, n) };
    recs.push(r.doc);
  }
  return { doc: docRecordMeet(recs), refusal: null };
}
/* The opener record a navigation of instance `eng` is bounded by (lib/document-record.js's docRecordOpenerOf):
   a document of `eng`'s answers its record, one another instance holds answers false. */
function engineOpenerLookup(eng) {
  return function (n) {
    if (eng.openers.has(n)) return eng.openers.get(n);
    return hostHolderOf(n) === null ? undefined : false;
  };
}
/* The records a provisioning notice of instance `eng` is judged by. `issuer` is the issuing document (HTML
   §7.4.2.2's sourceDocument, or the declaring document of a route), whose record the load is decided under.
   `minted` is the new document's name for a navigation, which the engine mints from the navigable's current
   document; that document's record joins the issuer's in `from`, so the new document keeps every flag and mark
   its navigable carries. A refusal names whichever of the two failed. */
function issuedRecords(eng, issuer, minted) {
  const iss = askerRecord(eng, [issuer], "issuer");
  if (iss.refusal) return { refusal: iss.refusal };
  if (minted === null) return { issuerDoc: iss.doc, from: iss.doc, refusal: null };
  const outgoing = docRecordNameParent(minted);
  if (outgoing === null) return { refusal: docRecordAskerRefusal("document", "not-minted", minted) };
  const nav = askerRecord(eng, [outgoing], "navigable");
  if (nav.refusal) return { refusal: nav.refusal };
  return { issuerDoc: iss.doc, from: docRecordJoin(iss.doc, nav.doc), refusal: null };
}
/* A top-level document provisioned by a navigation (a swap, a navigation into a peer, a declared route): the
   record `from` (issuedRecords) joined with the navigation's own words and no flags of its own, so it keeps the
   navigable's flags, inherits every mark above it and can never be stronger than the documents that issued it. */
function childDocRecord(from, provenance, driven, pinned) {
  return docRecordJoin(from, docRecord(provenance, driven, pinned, NAVIGABLE_NOTICE.sandbox.none,
                                       docRecordAddressSpace(from)));
}
/* A document's principal: HTML §7.3.2.1 "Creating browsing contexts"' determine the origin returns a new opaque
   origin when its record carries the sandboxed origin browsing context flag, and `origin` (the URL's, or the
   browser-stated one) otherwise, which is itself a new opaque origin, serialized "null", for a URL such as a
   data: URL (URL §4.7 "Origin"). Either opaque one is `_senderOrigin`'s "null:<uuid>" shape, unique per
   document, so clusterKeyOf never joins it to an instance and navigationCarriesSession gives it no session;
   the engine computes "null" from the same flag set and URL (main.c's qjs_init). */
function principalOf(doc, origin) {
  return docRecordOpaque(doc) || origin === "null" ? "null:" + crypto.randomUUID() : origin;
}
/* An entry's flag set. One written before the field has the empty set: until the zone recorded flags, every
   persisted document was a top-level root or seed created with `none`. */
function frontierSandbox(e) {
  return e.sandbox !== undefined ? e.sandbox : NAVIGABLE_NOTICE.sandbox.none;
}
/* An entry's IP address space (Local Network Access §3.5). One written before the field is a root or seed, as
   above, whose container was a new one: its response's space. */
function frontierAddressSpace(e) {
  return e.addressSpace !== undefined ? e.addressSpace : self.safeFetchDocumentAddressSpace(e.sourceUrl, null);
}
/* An entry's arrival. One written before the field states its grade and the strict marks: an `observed` entry is
   the ambient seed, which no drive or pin reached, and any other grade takes `driven` and `pinned`, the words
   that refuse rather than fire. */
function frontierArrival(e) {
  if (e.arrival !== undefined) return e.arrival;
  const reach = frontierProvenance(e);
  const none = NAVIGABLE_NOTICE.sandbox.none;
  const space = frontierAddressSpace(e);
  return docRecordArrival(reach === PROVENANCE_OBSERVED ? docRecord(reach, DRIVEN_NO, "unpinned", none, space)
                                                         : docRecord(reach, DRIVEN_YES, "pinned", none, space));
}
function frontierDoc(e, when) {
  frontierPlace(e, when);
  /* And it still holds its document: a shed entry's bytes are on the network, and the two states are told
     apart by the `shed` field, not by whether a read found bytes. */
  DCHECK(e.shed !== true,
         "a cold-tier entry " + when + " as a document while it is SHED — its bytes were discarded against a " +
         "proved re-fetch and live on the network, so this record must be re-derived before it is read as a " +
         "document rather than parsed as one that is missing");
  DCHECK(e.html instanceof Uint8Array || typeof e.html === "string",
         "a cold-tier entry " + when + " as neither a byte sequence nor characters — the parked document " +
         "would rebuild as a page that parses to nothing, which reads as an origin whose flows found nothing");
  DCHECK(typeof e.code === "string",
         "a cold-tier entry " + when + " with no script inventory — it is the empty string when the document " +
         "carries its own scripts, which is a different thing from absent");
  return e;
}
/* ── Residency: a configured share of this person's device, and the one order ─────────────────────────
   This is not a quota handler: the extension declares `unlimitedStorage` (manifest.json), which exempts the
   extension origin (where SECURITY.md puts every persisted byte) from the origin quota. What remains is a
   preference: how much of this person's device the tool may use, defaulted from the device and adjustable.
   A share is not a cap: it decides how much finished work is stored rather than re-derived, never whether
   work happens. No flow, recipe or exploration is dropped; frontier membership is untouched at every share.
   There is no eviction policy beside the WFQ: the store keeps the document half of the highest-weight entries
   (by `level1Key`, the same order `_hostOps.evictee` and admission ask) until the share is spent. The
   store holds no clock, so the order cannot become a recency cap. Nothing polls or sweeps.
   A shed entry keeps recipes, counters, address, principal and policy: it resumes on the next visit as before
   (engineRoot seeds from `prior.recipes`) and cold-rehydrates by fetching its document back, so the
   re-derivability test is a tiebreak on what leaves first, not a gate. */
/* The share: bytes of this profile's disk the frontier's document halves may occupy, defaulted from
   `navigator.deviceMemory` (Device Memory §2 "The deviceMemory attribute", coarse and capped at 8), so a phone and a workstation each get a
   share they can afford. `chrome.system.memory` is not used because it would cost a new permission for a
   default the user can set. An absent `deviceMemory` is answered by the conservative constant. */
const FRONTIER_SHARE_UNKNOWN_DEVICE = 256 * 1024 * 1024;
const FRONTIER_SHARE_PER_DEVICE_GB = 64 * 1024 * 1024;
function frontierDefaultShare() {
  const gb = (typeof navigator !== "undefined") ? navigator.deviceMemory : undefined;
  if (typeof gb !== "number" || !(gb > 0)) return FRONTIER_SHARE_UNKNOWN_DEVICE;
  return Math.round(gb * FRONTIER_SHARE_PER_DEVICE_GB);
}
/* The configured value, held in the `prefs` store (this zone's state is IndexedDB, never
   `chrome.storage.local`), read once into `_frontierShare` and written by the popup's setting. `null` means
   not read yet; zero is a legitimate setting (store no documents, re-derive everything). */
let _frontierShare = null;
async function frontierShare() {
  if (_frontierShare !== null) return _frontierShare;
  const stored = await frontierPref("share");
  /* Validated, not trusted: the popup writes it, and a non-number would make every comparison false, which
     reads as an unlimited share. */
  DCHECK(stored === null || (typeof stored === "number" && Number.isFinite(stored) && stored >= 0),
         "the frontier's stored share is not a byte count (" + String(stored) + ") — every residency " +
         "comparison against it would be false, which is an UNLIMITED store wearing the appearance of a " +
         "configured one, and the user's setting silently doing nothing");
  _frontierShare = (typeof stored === "number" && Number.isFinite(stored) && stored >= 0)
                   ? stored : frontierDefaultShare();
  return _frontierShare;
}
/* The size of an entry's document half, the only part residency can give back. Recipes, counters and address
   stay at every share. Characters count two bytes (the store holds UTF-16); the number exists for the order it
   induces, not as an accounting identity with the disk. */
function frontierDocBytes(e) {
  if (e.shed === true) return 0;
  return e.docBytes + e.code.length * 2;
}
/* The tiebreak for which tail entry gives up its document first: a cheap test on the record, not a proof,
   because a wrong answer costs only a re-fetch (a residue whose document does not come back keeps its recipes
   and visit-resume and is counted in `_frontierStats.stranded`). Not re-derivable: a non-http(s) address
   (nothing re-requests it), a principal the address does not state (an opaque or sandboxed document), or a
   caller-assembled script inventory. Those are the only copies and go last, not never. */
function frontierRederivable(e) {
  if (typeof self.safeFetch !== "function") return false;
  let u = null;
  try { u = new URL(e.sourceUrl); } catch (_) { return false; }
  if (u.protocol !== "http:" && u.protocol !== "https:") return false;
  if (e.origin !== u.origin) return false;
  /* A shed entry's inventory went with its document, and it was shed because this test passed. */
  if (e.shed === true) return true;
  return e.code === "";
}
/* The store's decisions, counted where they are made, since a share cannot be observed by its outcome:
   documents shed, re-derived, and stranded (shed and then not fetchable back, the costly direction), plus
   `overShare`, the bytes residency refused to give back because they were the only copy of a residue. */
/* `docBytes` is what the document halves occupy with each body counted once; `docBytesNamed` what they would
   occupy if every entry held its own copy, so the pair is what storing a body once by digest saved. */
const _frontierStats = { shed: 0, rederived: 0, stranded: 0, docBytes: 0, docBytesNamed: 0, overShare: 0 };
/* Residency, restored at the one door that can break it. "The stored document halves fit the share" is an
   invariant of the store and only `frontierPut` can falsify it, so it is re-established there; there is no
   pressure loop or trigger.
   Sort by `level1Key`, keep documents from the top while the share lasts, shed the rest; within the tail
   the re-derivable leave first. If what remains over the line is all unrecoverable, residency stops and the
   overage is reported: a disk preference is not worth the only copy of work nobody can recompute. */
async function frontierResidency() {
  const share = await frontierShare();
  const rows = [];
  /* How many resident entries name each body: a body's bytes are freed when the last of them is shed. */
  const names = new Map();
  let total = 0, named = 0;
  for (const row of (await frontierIndex()).values()) {
    named += row.bytes;
    if (row.doc !== null) rows.push(row);
    if (row.doc === null) { total += row.bytes; continue; }
    total += row.bytes - row.docBytes;
    if (!names.has(row.doc)) total += row.docBytes;
    names.set(row.doc, (names.has(row.doc) ? names.get(row.doc) : 0) + 1);
  }
  /* The HTTP cache's stored bodies no frontier entry names. They rank below every frontier entry, since a
     refetch re-derives each one, so over the share they are shed first, all together. */
  const cacheDocs = new Map();
  for (const row of _httpCacheDisk.values())
    for (const [d, b] of row) if (!names.has(d)) cacheDocs.set(d, b);
  let cacheBytes = 0;
  for (const b of cacheDocs.values()) cacheBytes += b;
  total += cacheBytes;
  _frontierStats.docBytes = total;
  _frontierStats.docBytesNamed = named;
  _frontierStats.overShare = 0;
  if (total <= share) return;
  if (_httpCacheDisk.size > 0) {
    await httpCacheShed();
    total -= cacheBytes;
    _frontierStats.docBytes = total;
    if (total <= share) return;
  }
  /* Highest in the one order first, so the share is spent on what it values most. */
  const tree = level1Tree(_pool, _frontierIndex);
  const k = new Map(rows.map((row) => [row, level1Key(tree, row.sourceUrl, row, true, LEVEL1_DOOR.cold)]));
  rows.sort((a, b) => level1Ahead(k.get(b), k.get(a)) ? 1 : level1Ahead(k.get(a), k.get(b)) ? -1 : 0);
  /* The resident set is a strict prefix of the order: the first entry that does not fit ends it. A best fit
     would keep a small low-weight document over a large high-weight one, a second policy nobody chose. */
  /* A body a kept entry already holds costs the next entry naming it only its inventory. */
  let kept = 0;
  let cut = rows.length;
  const keptDocs = new Set();
  for (let i = 0; i < rows.length; i++) {
    const cost = rows[i].bytes - (keptDocs.has(rows[i].doc) ? rows[i].docBytes : 0);
    if (kept + cost > share) { cut = i; break; }
    kept += cost;
    keptDocs.add(rows[i].doc);
  }
  /* What is shed is a body, with every tail entry naming it: an entry whose body a kept entry also names frees
     nothing by leaving, and a body is shed only when every entry naming it can fetch it back, since otherwise it
     is the only copy of an unrecomputable document. A unit is re-derivable when all its entries are. */
  const units = new Map();
  for (const row of rows.slice(cut)) {
    if (keptDocs.has(row.doc)) continue;
    if (!units.has(row.doc)) units.set(row.doc, { rows: [], rederivable: true });
    units.get(row.doc).rows.push(row);
    units.get(row.doc).rederivable = units.get(row.doc).rederivable && row.rederivable;
  }
  const tail = [...units.values()];
  /* The tiebreak orders the tail and changes nothing about who is in it. */
  tail.sort((a, b) => (a.rederivable === b.rederivable) ? 0 : (a.rederivable ? -1 : 1));
  for (const unit of tail) {
    if (total <= share) break;
    if (!unit.rederivable) {
      /* The sort put every re-derivable unit first, so the rest of the tail is unrecoverable; stop here. */
      _frontierStats.docBytes = total;
      _frontierStats.overShare = total - share;
      return;
    }
    for (const row of unit.rows) {
      const e = await frontierGet(row.key);
      DCHECK(e, "the cold tier's ranking view named an entry the store does not hold while residency was being " +
                "restored — this zone is the store's only writer, so a row with no entry is the projection and " +
                "the store having drifted apart, and shedding would be deciding about a residue nobody can read");
      /* `provenance` goes through frontierProvenance, so an entry predating the field leaves stating the word
         its population supports. */
      const shed = { key: e.key, sourceUrl: e.sourceUrl, topLevelUrl: e.topLevelUrl, origin: e.origin,
                     responseHeaders: e.responseHeaders, recipes: e.recipes, value: e.value, pulls: e.pulls, at: e.at,
                     credentialed: e.credentialed, provenance: frontierProvenance(e), arrival: frontierArrival(e),
                     sandbox: frontierSandbox(e), addressSpace: frontierAddressSpace(e), shed: true };
      frontierRecord(shed, "was shed to the configured share");
      await frontierWrite(e.key, shed, undefined);
      if (_frontierIndexBuilt) _frontierIndex.set(e.key, frontierRow(shed));
      /* The inventory goes with the entry; the body with its last name, which the unit's last entry is. */
      total -= row.bytes - row.docBytes;
      names.set(row.doc, names.get(row.doc) - 1);
      if (names.get(row.doc) === 0) total -= row.docBytes;
      _frontierStats.shed++;
    }
    DCHECK(names.get(unit.rows[0].doc) === 0,
           "a body was shed with a resident entry still naming it — a unit is every entry naming a body no kept " +
           "entry names, so its last shed is the body's last reference");
  }
  _frontierStats.docBytes = total;
}
/* The one record door, routing on the `shed` field rather than on whether a read found bytes. The document
   half and `shed` are complements, asserted together, so a self-contradicting record crashes here instead of
   parsing to nothing a session later. An absent `shed` means "not shed": entries predating the field hold
   their document. */
function frontierRecord(e, when) {
  frontierPlace(e, when);
  /* The account the Level-1 order is made of, asserted at the door it is written through. */
  level1AccountCheck(e, "a cold-tier entry " + when);
  DCHECK((e.doc !== undefined) !== (e.shed === true),
         "a cold-tier entry " + when + " claiming `shed=" + String(e.shed) + "` while its document half is " +
         (e.doc === undefined ? "absent" : "present") + " — the two are complements, and a record that " +
         "disagrees with itself is read as a document by one consumer and as a re-fetchable residue by another");
  DCHECK(e.html === undefined,
         "a stored cold-tier entry " + when + " holds its document inline — the store names a document by its " +
         "digest (lib/body-store.js), and frontierBodiesReady converts every entry an older build wrote");
  DCHECK(e.crash === undefined ||
         (e.crash !== null && typeof e.crash === "object" && typeof e.crash.program === "string" &&
          /^[0-9a-f]{64}$/.test(e.crash.program) && typeof e.crash.why === "string" && e.crash.why !== ""),
         "a cold-tier entry " + when + " records a crash without the engine it happened under or its cause (" +
         JSON.stringify(e.crash) + ") — the program digest is what decides whether re-running it is news");
  DCHECK(e.rest === undefined ||
         (e.rest !== null && typeof e.rest === "object" && typeof e.rest.program === "string" &&
          /^[0-9a-f]{64}$/.test(e.rest.program) && typeof e.rest.policy === "string"),
         "a cold-tier entry " + when + " rests without the program and egress policy it rests under (" +
         JSON.stringify(e.rest) + ") — those two are what can move a residue whose every member was refused");
  DCHECK(e.stranded !== true || e.shed === true,
         "a cold-tier entry " + when + " marked STRANDED while it still holds its document — stranded means a " +
         "shed entry's proved re-derivation stopped working, and an entry with its bytes has nothing to " +
         "re-derive");
  if (e.shed === true) {
    DCHECK(e.code === undefined,
           "a SHED cold-tier entry " + when + " still carrying a script inventory — the shed discards the " +
           "document HALF, and half a discard leaves the quota it was taken to relieve exactly where it was");
    DCHECK(typeof e.recipes === "string" && e.recipes !== "",
           "a SHED cold-tier entry " + when + " with no recipes — the recipes are the whole of what a shed " +
           "keeps, so shedding an entry down to nothing is the reset this category exists instead of");
    return e;
  }
  DCHECK(typeof e.doc === "string" && /^[0-9a-f]{64}$/.test(e.doc) && Number.isInteger(e.docBytes) &&
         e.docBytes >= 0 && typeof e.code === "string",
         "a cold-tier entry " + when + " names its document by no digest, or with no size or script inventory");
  return e;
}
/* The cold tier's edges surface failure instead of answering empty, since an empty answer is exactly what a
   first visit looks like and the continuous frontier would silently become per-session. A DCHECK, not a CHECK:
   in release a user whose profile storage is unavailable loses cross-session resume (degraded, not wrong);
   in dev it is this codebase's edge failing. */
function frontierFail(op, err) {
  DFAIL("the cross-session frontier's IndexedDB " + op + " failed (" + String((err && err.message) || err) +
        ") — the ONE continuous frontier is persisted there, and an empty answer from this edge is " +
        "indistinguishable from a page that has never been visited");
}
async function frontierGet(key) {
  try {
    await frontierBodiesReady();
    const db = await idbOpen();
    return await new Promise((res, rej) => { const t = db.transaction("frontier").objectStore("frontier").get(key); t.onsuccess = () => res(t.result || null); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("read", e); return null; }
}
/* The cold tier's lookup, which answers why a key missed and not only whether: nothing was ever parked for
   this document, something is parked at this address under another bundle id (the key working), or this key
   is stored and the read did not return it (the only defect).
   The key is composed `address + "|" + identity + "|" + bundle` (frontierKeyOf), so all entries for one address
   are a contiguous run of the store's key order and a bounded `getAllKeys` finds them without deserializing
   any record or adding an index.
   Both reads ride one transaction, a consistent snapshot against this zone's writes, so "get absent, scan
   present" is the store contradicting itself, never an interleaving, which is what entitles the DCHECK.
   A `keys` of null is the edge failing, not an empty store. */
function frontierAddressRange(sourceUrl) {
  return IDBKeyRange.bound(sourceUrl + "|", sourceUrl + "|￿");
}
/* A document's frontier key: its address, its identity (frontierIdentityTag: the cookie partition key and
   whether its load carries the session) and its bundle id. Two documents at one address under different
   partitions or credential states park and resume apart. Its caller has asked the address's host. */
function frontierKeyOf(address, origin, credentialed, bundle) {
  DCHECK(typeof bundle === "string" && /^[0-9a-z]+$/.test(bundle),
         "a frontier key was composed with the bundle id " + JSON.stringify(bundle) + ", not a base-36 number");
  return address + "|" + frontierIdentityTag(address, origin, credentialed) + "|" + bundle;
}
/* The three halves of a stored key. The bundle id is base-36 and the identity is URI-encoded, so neither holds
   `|` and the last two are the joints; a stored address that itself ends in `|<x>|<y>` sorts inside the
   range and is rejected by the caller's comparison. */
function frontierKeyAddress(key) {
  const k = String(key), j = k.lastIndexOf("|"), i = j < 0 ? -1 : k.lastIndexOf("|", j - 1);
  return i < 0 ? null : k.slice(0, i);
}
function frontierKeyIdentity(key) {
  const k = String(key), j = k.lastIndexOf("|"), i = j < 0 ? -1 : k.lastIndexOf("|", j - 1);
  return i < 0 ? null : k.slice(i + 1, j);
}
function frontierKeyBundle(key) {
  const k = String(key), j = k.lastIndexOf("|");
  return j < 0 ? null : k.slice(j + 1);
}
async function frontierLookup(key, sourceUrl) {
  try {
    await frontierBodiesReady();
    const db = await idbOpen();
    return await new Promise((res, rej) => {
      const tx = db.transaction("frontier");
      const s = tx.objectStore("frontier");
      const g = s.get(key);
      const a = s.getAllKeys(frontierAddressRange(sourceUrl));
      let entry = null; let keys = [];
      g.onsuccess = () => { entry = g.result || null; };
      a.onsuccess = () => { keys = a.result || []; };
      /* Settled on the transaction, so both halves are delivered from one snapshot. */
      tx.oncomplete = () => res({ entry: entry, keys: keys.filter((k) => frontierKeyAddress(k) === sourceUrl) });
      tx.onerror = () => rej(tx.error || g.error || a.error);
      tx.onabort = () => rej(tx.error || g.error || a.error);
    });
  } catch (e) { RETHROW_FATAL(e); frontierFail("read", e); return { entry: null, keys: null }; }
}
/* What the lookup met, as one word from a closed set; each is a positive statement:
     `not-asked`     — this document does not persist a residue, so the store was not asked.
     `unreadable`    — the store's read edge failed (frontierFail aborts in dev); not "never visited".
     `unvisited`     — no entry for this address under any bundle id: a first visit.
     `other-bundle`  — entries for this address and identity, none under this bundle id: the key doing its job.
                       It names the observation; a redeploy is the usual cause but is an inference.
     `other-identity` — entries for this address, none under this document's identity (another partition or
                       credential state): a different document, which resumes none of them.
     `unread`        — this key is stored and the get did not answer it; the only defect of the six.
     `hit`           — the key answered and the session resumes from it.
     `crashed`       — the key answered with a residue whose resume aborted this engine: the session boots
                       fresh instead, since re-running it unchanged repeats the abort (frontierRecordCrash).
   `others` is the evidence beside the word, not entailed by it (a hit can have older deploys' residues
   parked beside it). It is `null` on the two arms that counted nothing. */
const COLD_LOOKUP = ["hit", "crashed", "unvisited", "other-bundle", "other-identity", "unread", "not-asked",
                     "unreadable"];
function coldLookupOf(fkey, look) {
  if (look === null) return { state: "not-asked", others: null };
  if (look.keys === null) return { state: "unreadable", others: null };
  const held = look.keys.indexOf(fkey) >= 0;
  const others = look.keys.length - (held ? 1 : 0);
  /* A record came back for this key, so the scan of its address must name it; otherwise the range or separator
     split the key differently from engineRoot and `others` counts the wrong address. */
  DCHECK(!look.entry || held,
         "the cold tier answered the frontier key `" + fkey + "` with a record while a key scan of that " +
         "document's own address did not name it — the two halves are one read transaction, so this is the " +
         "address range or the separator split disagreeing with the key engineRoot composed, and the " +
         "sibling-entry count every other arm reports would be a count over a different address");
  if (look.entry) return { state: frontierCrashedHere(look.entry) ? "crashed" : "hit", others: others };
  if (held) {
    /* Reading (c): both halves are this zone's records under its keys in one snapshot, so this is the store
       contradicting itself. Release returns the word, leaving the state a miss leaves (`prior` null, the engine
       seeds a boot flow), so the run still says which of the six it met. */
    DFAIL("the cross-session frontier enumerates the key `" + fkey + "` for this document's address and " +
          "answered a read of that same key, in the same transaction, with nothing — this zone is the store's " +
          "only writer and both halves are one snapshot, so a parked residue is being kept out of the session " +
          "it belongs to and every visit to this document will re-explore from a boot flow for ever");
    return { state: "unread", others: others };
  }
  if (others === 0) return { state: "unvisited", others: others };
  const sameIdentity = look.keys.some((k) => frontierKeyIdentity(k) === frontierKeyIdentity(fkey));
  return { state: sameIdentity ? "other-bundle" : "other-identity", others: others };
}
/* The one write, settled when its transaction commits (idbWrite). It rejects on failure like every edge here;
   there is no quota arm (`unlimitedStorage`), and residency is asked by frontierPut after the write lands. The
   durability hint is the storage bucket's default: entries are rewritten on every park. */
function frontierWrite(key, entry, value) {
  const keep = !!(entry && entry.recipes);
  const doc = keep && entry.doc !== undefined ? entry.doc : null;
  DCHECK((doc === null) === (value === undefined),
         "a cold-tier write names a document digest without its bytes, or bytes without a digest");
  return idbWriteStores(["frontier", "bodies"], "default", (st) => {
    const g = st.frontier.get(key);
    g.onsuccess = () => {
      const old = g.result && g.result.doc !== undefined ? g.result.doc : null;
      if (old !== null && old !== doc) bodyUnref(st.bodies, old);
      if (doc !== null && doc !== old) bodyRef(st.bodies, doc, value);
      if (keep) st.frontier.put(entry, key); else st.frontier.delete(key);
    };
  });
}
/* The stored form of an in-memory entry (frontierDoc's: its document inline): the document named by digest. */
async function frontierStoredForm(entry) {
  frontierDoc(entry, "was written");
  const stored = Object.assign({}, entry);
  delete stored.html;
  stored.doc = await bodyDigest(entry.html);
  stored.docBytes = bodyBytes(entry.html);
  return stored;
}
async function frontierPut(key, entry) {
  if (entry && entry.recipes) await level1AskSites([entry.sourceUrl]);
  try {
    await frontierBodiesReady();
    if (entry && entry.recipes) {
      const stored = await frontierStoredForm(entry);
      frontierRecord(stored, "was written");   // before the edge: the grammar, not the storage
      await frontierWrite(key, stored, entry.html);
      entry = stored;
    } else {
      await frontierWrite(key, null, undefined);
    }
    /* The ranking view moves with the store, on the one door that writes it, once the write landed; an unbuilt
       index is skipped because it will be built from the store, which then contains this. */
    if (_frontierIndexBuilt) { if (entry && entry.recipes) _frontierIndex.set(key, frontierRow(entry)); else _frontierIndex.delete(key); }
    /* Residency is re-established after the ranking view moved, so it asks the order the store is in. A store
       inside its share does one comparison. */
    await frontierResidency();
  } catch (e) { RETHROW_FATAL(e); frontierFail("write", e); }
}
/* Record on the stored entry `prior` that resuming it aborted this engine with the root line `why`. The recipes,
   document and account are kept: the residue is evidence, and a later engine may run it. */
async function frontierRecordCrash(prior, why) {
  const program = await programDigest();
  DCHECK(prior && typeof prior.key === "string" && typeof prior.recipes === "string" && prior.recipes !== "",
         "a crash was recorded against no stored residue — only a run that resumed recipes records one");
  DCHECK(!frontierCrashedHere(prior),
         "a residue already recorded as aborting this engine was resumed and aborted it again — engineRoot boots " +
         "such a document fresh and the Level-1 walk excludes its row, so a third door re-admitted it");
  DCHECK(typeof why === "string" && why !== "",
         "a crashed run left no root line to record — engineCrash writes the @E line every crash carries");
  const entry = Object.assign({}, prior, { crash: { program: program, why: why } });
  frontierRecord(entry, "recorded a crash");
  try {
    await frontierBodiesReady();
    await frontierWrite(entry.key, entry, entry.shed === true ? undefined : await frontierBody(entry.doc));
    if (_frontierIndexBuilt) _frontierIndex.set(entry.key, frontierRow(entry));
  } catch (e) { RETHROW_FATAL(e); frontierFail("write", e); }
  DCHECK(!_frontierIndexBuilt || _frontierIndex.get(entry.key).crashed === true,
         "a recorded crash did not reach the ranking view — the Level-1 walk would offer the residue again");
}
/* A stored document's bytes, by the digest its entry names. */
async function frontierBody(digest) {
  try {
    const db = await idbOpen();
    const rec = await new Promise((res, rej) => {
      const t = db.transaction("bodies").objectStore("bodies").get(digest);
      t.onsuccess = () => res(t.result); t.onerror = () => rej(t.error);
    });
    DCHECK(rec !== undefined && Number.isInteger(rec.refs) && rec.refs >= 1,
           "a cold-tier entry names a body the store does not hold — the entry and its reference are written in " +
           "one transaction, so the two have parted");
    return rec === undefined ? null : rec.value;
  } catch (e) { RETHROW_FATAL(e); frontierFail("body read", e); return null; }
}
/* The one reader of the inline document form: entries an older build wrote with `html` are converted to a
   digest and a body reference, each in its own transaction, before any other edge reads the store. */
let _frontierBodiesConverted = null;
function frontierBodiesReady() {
  if (_frontierBodiesConverted === null) _frontierBodiesConverted = (async () => {
    try {
      const db = await idbOpen();
      const inline = await new Promise((res, rej) => {
        const t = db.transaction("frontier").objectStore("frontier").getAll();
        t.onsuccess = () => res((t.result || []).filter((e) => e && e.html !== undefined));
        t.onerror = () => rej(t.error);
      });
      for (const e of inline) {
        const stored = await frontierStoredForm(e);
        await frontierWrite(e.key, stored, e.html);
      }
      await frontierKeysConverted(db);
    } catch (e) { RETHROW_FATAL(e); frontierFail("body conversion", e); }
  })();
  return _frontierBodiesConverted;
}
/* The one reader of the address-only key: an entry an older build keyed `address|bundle` moves to its
   identity key (frontierKeyOf over its stored principal and session state), in one transaction that asserts
   the new key is free. Its body is named by the same one entry, so no reference count moves. */
async function frontierKeysConverted(db) {
  const all = await new Promise((res, rej) => {
    const t = db.transaction("frontier").objectStore("frontier").getAll();
    t.onsuccess = () => res(t.result || []); t.onerror = () => rej(t.error);
  });
  await level1AskSites([].concat(...all.map((e) => [e.sourceUrl, e.origin])).filter((u) => u.indexOf("null") !== 0));
  for (const e of all) {
    const want = frontierKeyOf(e.sourceUrl, e.origin, e.credentialed, frontierKeyBundle(e.key));
    if (want === e.key) continue;
    await idbWriteStores(["frontier"], "default", (st) => {
      const g = st.frontier.get(want);
      g.onsuccess = () => {
        DCHECK(g.result === undefined,
               "an entry keyed " + e.key + " is being moved to its identity key " + want + ", which another " +
               "entry already holds — one document's residue would replace another's");
        st.frontier.put(Object.assign({}, e, { key: want }), want);
        st.frontier.delete(e.key);
      };
    });
  }
}
async function frontierAll() {
  try {
    await frontierBodiesReady();
    const db = await idbOpen();
    return await new Promise((res, rej) => { const t = db.transaction("frontier").objectStore("frontier").getAll(); t.onsuccess = () => res(t.result || []); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("scan", e); return []; }
}
/* Level 1: one order over documents, hot, parked and waiting alike, over a two-level tree, site then document
   (level1Tree, level1Key). A pull is one quantum of work, so an account is counts: `value`, the razor-weighted
   novel value its instances' flows landed (main.c's qjs_level1_account, solver/novelty.h's weights), and `pulls`,
   the quanta it was served. A node's index is the Poisson KL upper confidence bound on its value per pull
   (Garivier and Cappé, "The KL-UCB Algorithm for Bounded Stochastic Bandits and Beyond", 2011, with the Poisson
   divergence d(p, q) = q − p + p ln(p / q)) over its pulls P against its parent's N:
     max { q >= v / P : P d(v / P, q) <= ln N },   and +Infinity at P = 0,
   with N every site's pulls for a site and the site's own for a document. Counts carry no unit, so the order is
   the same at any scale of the work unit, and the bound needs no reward ceiling. A never-served node is ahead of
   every served one, so every new site gets its first quantum before any site deepens. A barren node's index is
   ln N / P, so it falls below a productive sibling's rate r once P > ln N / r.
   Discounted by the product's growth (level1At): an account is held at the product size it was last credited
   at, and is scaled by that size over the current one, value and pulls alike. A find is consumed, so value the
   product has since absorbed from anywhere stops predicting this document's yield, and its uncertainty comes
   back with the shrunken pulls. Parameter-free: the scale is the product's own count. */
const LEVEL1_UNSERVED = Object.freeze({ value: 0, pulls: 0, at: 1 });
/* The product's size as the discount reads it: one plus the identities lib/novelty-set.js holds, so it is at least
   1 and only grows within a zone's life. */
function level1ProductSize() {
  return 1 + noveltyProductSize();
}
/* Asserted at every reader, so the messages are composed only on the failing arm: the order reads accounts at
   every rank of every member, and an eager message would cost more than the arithmetic it guards. */
function level1AccountCheck(a, what) {
  if (!(a && typeof a.value === "number" && Number.isFinite(a.value) && a.value >= 0))
    DFAIL(what + " carries no Level-1 value (`" + String(a && a.value) + "`) — `value` is a sum of positive " +
          "credits, and without it the order ranks this document by its optimism alone");
  if (!(typeof a.pulls === "number" && Number.isFinite(a.pulls) && a.pulls >= 0))
    DFAIL(what + " carries no Level-1 service (`" + String(a.pulls) + "`) — `pulls` is the divisor of the rate " +
          "and of the bound, and an absent one ranks a document served for an hour as a new one");
  if (!(typeof a.at === "number" && Number.isFinite(a.at) && a.at >= 1))
    DFAIL(what + " carries no product size it was credited at (`" + String(a.at) + "`)");
}
/* `a` held at product size `g`: value and pulls scaled by a.at / g. */
function level1At(a, g) {
  level1AccountCheck(a, "a Level-1 account being discounted");
  if (!(g >= a.at))
    DFAIL("a Level-1 account was credited at product size " + a.at + " and is read at " + g + " — the product " +
          "only grows within a zone's life, so this account came from another product");
  const f = a.at / g;
  return { value: a.value * f, pulls: a.pulls * f, at: g };
}
function level1Sum(a, b, g) {
  const x = level1At(a, g), y = level1At(b, g);
  return { value: x.value + y.value, pulls: x.pulls + y.pulls, at: g };
}
function level1Credit(a, dValue, dPulls, g) {
  if (!(dValue >= 0 && dPulls >= 0))
    DFAIL("a Level-1 credit of " + dValue + " value over " + dPulls + " pulls — both are differences of an " +
          "instance's lifetime counters, which only climb");
  const x = level1At(a, g);
  return { value: x.value + dValue, pulls: x.pulls + dPulls, at: g };
}
/* The largest q >= lam with q − lam + lam ln(lam / q) <= beta: Newton's method on the divergence, which is convex
   and rising in q above lam, started at lam + beta + sqrt(beta^2 + 2 lam beta), where d >= (q − lam)^2 / (2q)
   already reaches beta. From that side every iterate stays at or above the root and falls, so the loop stops
   when one fails to fall, which is the root to the double. At lam = 0 the divergence is q, so the root is beta. */
function level1KlUcb(lam, beta) {
  if (beta <= 0) return lam;
  if (lam === 0) return beta;
  let q = lam + beta + Math.sqrt(beta * beta + 2 * lam * beta);
  for (;;) {
    const next = q - (q - lam + lam * Math.log(lam / q) - beta) / (1 - lam / q);
    if (!(next < q)) return q;
    q = next;
  }
}
function level1Index(a, parentPulls) {
  level1AccountCheck(a, "a ranked Level-1 work item");
  /* Exact: a parent's pulls are a rounded sum of non-negative terms that include this one, and rounded addition
     of non-negatives is monotone. */
  if (!(parentPulls >= a.pulls))
    DFAIL("the Level-1 parent's pulls (" + parentPulls + ") are below one child's own (" + a.pulls +
          ") — a parent's account is the sum of its children's, so a member outside it was ranked");
  if (a.pulls === 0) return Infinity;
  const w = level1KlUcb(a.value / a.pulls, parentPulls > 1 ? Math.log(parentPulls) / a.pulls : 0);
  if (!Number.isFinite(w))
    DFAIL("a served Level-1 work item indexed " + w + " — the bound is finite over a positive service, so the " +
          "arithmetic above was given something else");
  return w;
}
/* The rate alone, which the Level-1 census reports beside the index; null for a document never served. */
function level1Rate(a) {
  return a.pulls === 0 ? null : a.value / a.pulls;
}
/* The entry points into Level 1's population, one per kind of member, each of which awaits level1AskSites over
   the member's address before the member can be ranked; level1SiteOf names the one whose member was not asked.
   The site is never stored on a member: Chrome's list changes with its version, which the memo is keyed by. */
const LEVEL1_DOOR = Object.freeze({
  cold: "a cold row (frontierIndex and frontierPut ask before they index; a shed or stranded rewrite keeps its " +
        "row's address)",
  instance: "an instance (engineRoot asks before its first account)",
  doc: "a waiting document (astDispatch asks before it queues)",
  seed: "a declared route (hostNotice asks before it registers)",
});
function level1AskSites(urls) {
  return siteAsk(siteHostsOf(urls));
}
/* A document's site: HTML §7.1.1.1 "obtain a site" through lib/site.js's siteOf. An opaque origin is a site
   only of itself, so its document is its own site, named by its address. */
function level1SiteOf(url, door) {
  DCHECK(siteAnswered(url),
         "Level 1 ranked " + door + " at " + url + " whose host was never asked — that entry point awaits " +
         "siteAsk (level1AskSites) before the member joins the population, so a ranked member is an asked one");
  const site = siteOf(url);
  return site === null ? "opaque " + url : site;
}
/* The tree's accounts over the population this zone knows, held at the product's current size: each cold row's
   stored account plus each instance's own account since it booted (a resumed instance's stored share is its row,
   so nothing is counted twice), summed per site. `totalPulls` is the root's service, the sum over sites. An
   instance that has not reported has served nothing. */
function level1Tree(resident, idx) {
  const g = level1ProductSize();
  const sites = new Map();
  const add = (url, acct, door) => {
    const k = level1SiteOf(url, door), a = sites.get(k);
    sites.set(k, level1Sum(a === undefined ? LEVEL1_UNSERVED : a, acct, g));
  };
  for (const row of idx.values()) add(row.sourceUrl, row, LEVEL1_DOOR.cold);
  for (const e of resident)
    if (e.level1 !== undefined) add(e.msg.sourceUrl, e.level1.acct, LEVEL1_DOOR.instance);
  let totalPulls = 0;
  for (const a of sites.values()) totalPulls += a.pulls;
  return { sites: sites, totalPulls: totalPulls, g: g };
}
/* A document's place in the tree: its site, the site's index among sites, and its own index within the site.
   A document no member of which can run ranks last at both levels. */
function level1Key(tree, url, acct, runnable, door) {
  const site = level1SiteOf(url, door);
  if (!runnable) return { site: site, siteW: -Infinity, docW: -Infinity };
  const sa = tree.sites.get(site) || LEVEL1_UNSERVED;
  return { site: site, siteW: level1Index(level1At(sa, tree.g), tree.totalPulls),
           docW: level1Index(level1At(acct, tree.g), level1At(sa, tree.g).pulls) };
}
/* Whether `a` is strictly ahead of `b`: the document weight decides within one site, the site weight across. */
function level1Ahead(a, b) {
  return a.site === b.site ? a.docW > b.docW : a.siteW > b.siteW;
}
/* The cold tier's ranking view, a projection of the store. The Level-1 order asks two numbers of a parked
   frontier on every scheduler round, and `getAll` would deserialize every parked document to answer them.
   This zone is the store's only writer and frontierPut updates the view on its way through, so a row and an
   entry cannot disagree; it is built once per zone lifetime from the store. */
const _frontierIndex = new Map();   // key -> frontierRow(entry): { key, sourceUrl, value, pulls, at, shed, stranded, bytes, rederivable }
let _frontierIndexBuilt = false;
/* The engine this zone runs (renderer-host's rendererProgramDigest), read once per zone lifetime: a crash on a
   cold entry names the program it happened under, and only that program's re-run of the residue repeats it. */
let _programDigest = null;
async function programDigest() {
  if (_programDigest === null) _programDigest = await self.rendererProgramDigest();
  DCHECK(typeof _programDigest === "string" && /^[0-9a-f]{64}$/.test(_programDigest),
         "renderer-host named the engine it runs with something that is not a SHA-256 digest (" +
         String(_programDigest) + ")");
  return _programDigest;
}
/* The person's egress table in one canonical spelling (sorted at every level), so two tables granting the same
   acts compare equal whatever order they were granted in. */
function egressPolicyNow() {
  const t = self.safeFetchEgressTable();
  return JSON.stringify(Object.keys(t).sort().map((o) =>
    [o, Object.keys(t[o]).sort().map((sig) => [sig, t[o][sig].slice().sort()])]));
}
/* Whether a residue written because every member rests on a refusal still rests: nothing that could answer a
   refusal has changed, neither the engine this zone runs nor the egress policy the requests were judged under.
   A change of either is the host event the rest waits for, as FLOW_STEP_OWED waits for one inside a session. */
function frontierRestHolds(rest, policyNow) {
  DCHECK(_programDigest !== null,
         "a resting cold-tier row was ranked before this zone knew which engine it runs — frontierIndex asks " +
         "programDigest first");
  return rest.program === _programDigest && rest.policy === policyNow;
}
/* Whether `e` records a crash under the engine this zone runs. Asked only after programDigest has answered. */
function frontierCrashedHere(e) {
  if (e.crash === undefined) return false;
  DCHECK(_programDigest !== null,
         "a cold-tier entry recording a crash was read before this zone knew which engine it runs — " +
         "frontierIndex, engineRoot and frontierRecordCrash ask programDigest first");
  return e.crash.program === _programDigest;
}
function frontierRow(e) {
  DCHECK(typeof e.key === "string" && e.key !== "",
         "a cold-tier entry carries no key — it is the name the pool holds this item under while it ranks it, " +
         "and an entry the ranking cannot name is one it can neither admit nor exclude from being admitted twice");
  DCHECK(typeof e.sourceUrl === "string" && e.sourceUrl !== "",
         "a cold-tier entry carries no document address — the pool excludes a parked item whose document a " +
         "LIVE tab already holds, and an item with no address is one it would rehydrate into a second " +
         "instance beside the tab that is already running it");
  /* The principal and the session state are the rest of the entry's document identity (documentIdentityOf). */
  DCHECK(typeof e.origin === "string" && typeof e.credentialed === "boolean",
         "a cold-tier entry does not state its principal and whether its load carried the session (" +
         JSON.stringify([e.origin, e.credentialed]) + ") — they are the parked document's identity, which a " +
         "declared route of the same document lands on");
  /* The state bits the order reads, decided here as strict booleans (frontierRecord asserted what their absence
     means): `shed` has given its document up, `stranded` failed to re-derive it and is no longer a cold
     candidate (only a visit resumes it). The two residency numbers are computed here too, so residency never
     opens an entry to learn its size. */
  return { key: e.key, sourceUrl: e.sourceUrl, origin: e.origin, credentialed: e.credentialed,
           value: e.value, pulls: e.pulls, at: e.at,
           shed: e.shed === true, stranded: e.stranded === true, crashed: frontierCrashedHere(e),
           rest: e.rest === undefined ? null : e.rest,
           bytes: frontierDocBytes(e), rederivable: frontierRederivable(e),
           doc: e.shed === true ? null : e.doc, docBytes: e.shed === true ? 0 : e.docBytes };
}
async function frontierIndex() {
  if (_frontierIndexBuilt) return _frontierIndex;
  await programDigest();
  const all = await frontierAll();
  /* Rows restored from an earlier session name hosts this session has not asked. */
  await level1AskSites(all.filter((e) => e && typeof e.sourceUrl === "string").map((e) => e.sourceUrl));
  /* Another caller may have built the index while the ask was in flight. */
  if (_frontierIndexBuilt) return _frontierIndex;
  for (const e of all) {
    /* The store's invariant: frontierPut deletes an entry with no recipes, so every stored row has them. */
    DCHECK(e && typeof e.recipes === "string" && e.recipes !== "",
           "the cross-session frontier holds an entry with no parked recipes — frontierPut deletes rather than " +
           "storing one, so this is a residue whose flows were dropped between the park and the store");
    /* The record grammar on the way out, where an entry from an older build or a half-landed shed is met. */
    frontierRecord(e, "came back from the store");
    _frontierIndex.set(e.key, frontierRow(e));
  }
  _frontierIndexBuilt = true;
  return _frontierIndex;
}
/* The re-derivation of a shed entry's document: the same chokepoint, principal, address and session as the
   request the shed was judged against. It answers the document half, or null.
   It asks `navigationCarriesSession` like every document load in this file, so a document loads with the
   same credentials live and from the cold tier; the principal is the one parked with the recipe, so an empty
   one re-derives uncredentialed. `landed === e.sourceUrl` proves the bytes are this document.
   The headers that come back are used, not the parked ones: HTML §7.1.7 Policy containers builds the policy
   container from the response the document came from. A changed document is safe because engineRoot
   re-derives the key from what the engine parsed, so a redeployed bundle boots fresh.
   A failure is not a @WHY (the world may retire a route): it is recorded on the entry, which stops being a
   cold candidate, and in `_frontierStats.stranded`. The recipes are untouched, so the next visit resumes. */
async function frontierRederive(e) {
  /* The parked document speaks as itself for this one load and for nothing else, so it ends with the load. */
  const held = "held:frontier:" + e.key;
  try { return await _frontierRederive(e, held); }
  finally { speakAsDocumentEnded(held); }
}
async function _frontierRederive(e, held) {
  DCHECK(e.shed === true,
         "a cold-tier entry that still holds its document was sent to be re-derived — the fetch would replace " +
         "bytes this store already has, and the round would pay a network round trip to learn nothing");
  let r = null;
  if (frontierRederivable(e)) {
    /* A document: Fetch §2.2.5 Requests' `document` destination ("HTML's navigate algorithm (top-level
       only)"), parsed by the engine's own parser. */
    /* `provenance` and the document record are the entry's own parked words, since a re-derivation is the same
       load that stored it; a different word would re-fetch under a permission nobody granted. `pinned` is `unstated`:
       the store parks no witness mark, and `unpinned` would be a false claim for a forced address whose flow
       pinned a witness. The mark only refines a refusal's sentence (`_firingRefusal` chooses between `forced`
       and `forced-witness`), never the decision. */
    /* `credentials: "include"` is the navigation's own credentials mode (HTML §7.4.5 Populating a session
       history entry: "destination `document` … credentials mode `include`"). `credentialed` is this zone's
       willingness to spend the session; safe-fetch.js's `_credentialedOf` composes the two and requires a mode
       wherever the session pays. */
    /* `actor: "tool"`: no page asked for this load; the frontier did. It changes no outcome here, since the
       witness mark is `unstated`. */
    /* `mode: "navigate"`: the navigate algorithm's own request (Fetch §2.2.5 Requests). */
    try { r = await self.safeFetch(e.sourceUrl, { pageOrigin: e.origin,
                                                  destination: "document", mode: "navigate",
                                                  provenance: frontierProvenance(e),
                                                  doc: docRecordOfArrival(frontierArrival(e), frontierSandbox(e),
                                                                          frontierAddressSpace(e)),
                                                  actor: "tool", driven: "unstated",
                                                  pinned: "unstated",
                                                  credentials: "include",
                                                  credentialed: navigationCarriesSession(e.sourceUrl, e.origin),
                                                  /* A parked document is one only this zone holds. */
                                                  identity: navigationCarriesSession(e.sourceUrl, e.origin)
                                                    ? await navigationIdentity({ identity: await engineIdentity(e.origin, e.sourceUrl, held),
                                                                           referrer: "none", policy: null,
                                                                           request: null },
                                                                         e.sourceUrl, DOCUMENT_REQUEST_UNSTATED)
                                                    : null }); }
    catch (err) { RETHROW_FATAL(err); r = null; }
  }
  const landed = r && Array.isArray(r.urlList) && r.urlList.length ? r.urlList[r.urlList.length - 1] : null;
  if (r && r.ok && landed === e.sourceUrl && r.body instanceof Uint8Array && r.headers) {
    _frontierStats.rederived++;
    return { bytes: r.body, headers: r.headers };
  }
  const strandedEntry = { key: e.key, sourceUrl: e.sourceUrl, topLevelUrl: e.topLevelUrl, origin: e.origin,
                          responseHeaders: e.responseHeaders, recipes: e.recipes, value: e.value,
                          pulls: e.pulls, at: e.at, credentialed: e.credentialed, provenance: frontierProvenance(e),
                          arrival: frontierArrival(e), sandbox: frontierSandbox(e),
                          addressSpace: frontierAddressSpace(e), shed: true, stranded: true };
  frontierRecord(strandedEntry, "was stranded");
  await frontierWrite(e.key, strandedEntry);
  if (_frontierIndexBuilt) _frontierIndex.set(e.key, frontierRow(strandedEntry));
  _frontierStats.stranded++;
  return null;
}
/* ────────────────────────────────────────────────────────────────────────────────────────────────────
   Level 1 of the one attention: interleave the live document engines by a bound on novel value per pull,
   in slices, so no document or deep path monopolizes the thread. Each agent cluster is one wasm instance (SECURITY.md:
   one per (browsing-context group, origin)). The engine states its account (qjs_level1_account) and yields hot
   at the work quantum (qjs_step -> 2); the host ranks all live engines and every waiting and parked document by
   one order (level1Key, site then document), advances the winner one slice and re-ranks. The ranking does not ask: the
   account and the working set are recorded by engineRecordFacts at the end of every round, since an instance
   behind the frame boundary answers only by message.
   RAM is the floor: under HOT_RAM_BUDGET of summed working set new engines are admitted; at the floor
   `_hostOps.evictee` picks a resident engine to park (qjs_request_park, recipes to IndexedDB), and the cold
   tail is rehydrated into this same pool by admission. An engine awaiting a reply is `fetching` and skipped
   until its body lands, so a slow fetch on one document never stalls another.
   ──────────────────────────────────────────────────────────────────────────────────────────────────── */
// The hot working set is bounded by actual RAM, not an instance count (a light page's instance is a few MB, a
// heavy bundle's tens). Over the budget new documents wait as cold recipes and are pulled back by admission.
// This is a RAM floor, not a truncating bound; at least one document is always admitted.
const HOT_RAM_BUDGET = 512 * 1024 * 1024;   // bytes of summed live WASM memory before new engines wait
/* The hot working set, summed over what each instance last reported (engineRecordFacts writes it every round,
   the first time before the record reaches the pool), so an absent figure is a producer that stopped writing
   and crashes rather than counting as zero.
   `booting` reservations and `loading` seats (an admission whose document is still on the network: a declared
   route's navigation, HTML §7.4 Navigation and session history, or a shed residue's re-derivation) have
   reported nothing and are skipped; there
   is no honest figure for them, so the sum is a lower bound while one stands, and `_admissionHasHeadroom`
   counts them by state instead. Every other state must be classified here before it can be summed. */
function _residentBytes() {
  let b = 0;
  for (const e of _pool) {
    if (e.state === "booting" || e.state === "loading") continue;   // has reported nothing; _admissionHasHeadroom reads the state instead
    DCHECK(e.state === "hot" || e.state === "fetching",
           "an engine is in the pool in state `" + e.state + "`, which the RAM floor has no classification " +
           "for — it would be summed as if it had reported or skipped as if it never will, and neither is a " +
           "decision this function may take about a state it has never heard of");
    DCHECK(typeof e.residentBytes === "number",
           "a live engine is in the pool with no reported working set — the RAM floor is the sum of what each " +
           "instance last reported, and an engine missing from that sum is admission running against memory " +
           "that is already spent");
    b += e.residentBytes;
  }
  return b;
}
/* The reservations in flight: the positive form of "that sum is not complete yet". */
function _bootingCount() {
  let n = 0;
  for (const e of _pool) if (e.state === "booting") n++;
  return n;
}
/* The admissions whose document is still arriving, a separate fact from reservations: `_bootingCount`'s other
   readers (engineReserve's `peakBooting`, the Level-1 census's `booting` row and its `<= pool` assert) mean
   reservations by name. The two are disjoint subsets of the pool, which the census asserts. */
function _loadingCount() {
  let n = 0;
  for (const e of _pool) if (e.state === "loading") n++;
  return n;
}
/* Whether another instance may be built, asked in one place. A reservation or a loading seat blocks admission
   because the working set is then a lower bound, and admitting against it spends memory already spoken for.
   It is not a bound: provisioning and loads settle, hostSchedule's wait arm waits on the promise that clears
   it, the next round re-asks, and hot engines keep being stepped meanwhile.
   Named residual: a `loading` seat holds admission for as long as its load takes, so a body a remote party
   never ends closes admission for the session. Next diff builds a seat that does not hold admission: a cold
   seat carries its frontier key so `_bestCandidate` excludes its row, a seed seat carries its address into the
   walk's `live` set, and hostSchedule's wait arm becomes releasable by an arrival. Absence shows as
   rendererPoolProbe reporting `loadingSeats` of 1 or more with `waiting` climbing and `reservations.seeded`
   and `rehydrated` frozen, while the Level-1 census is still written every round with `loading` of 1 or
   more, no `cands` and `atFloor: 0`. */
function _admissionHasHeadroom() {
  if (_pool.length === 0) return true;     // always admit >= 1 so a lone document runs
  if (_bootingCount() > 0) return false;   // an instance that has not reported is a term the sum is missing
  if (_loadingCount() > 0) return false;   // a document already fetched for is memory this sum has not counted
  return !_atRamFloor();
}
/* The RAM floor, a different fact from whether another instance may be built: a reservation blocks admission
   but is no reason to evict. A wasm Memory never shrinks, so the floor once reached is permanent for the
   instances that reached it; it is answered by `_hostOps.evictee`, which parks one resident engine to the
   cold tier, not by refusing admission. */
function _atRamFloor() {
  return _residentBytes() >= HOT_RAM_BUDGET;
}
/* The one Level-1 candidate order: the highest-value work item with no instance, whatever its kind. A waiting
   document and a parked frontier compete in the same order (cold-tail resume is the same admission step), so
   a residue parked long ago competes on value while browsing continues.
   A waiting document ranks as unserved (weight 1.0): its key's bundle half comes from the engine's scan, so
   the host cannot yet know which residue it will resume and attributes none. engineRoot's frontierGet picks
   the residue up once the engine answers. An address a live document holds is not also a cold candidate,
   since the tab's engine resumes that residue itself; the exclusion is by address because the live half's key
   is not yet known, and exact because the address is in the key. */
/* The kinds of work item with no instance, declared once so that the walk below, `_candRanked`'s counting,
   `_level1Record`'s accounting assert and admission's dispatch on `kind` cannot disagree about the set. `kind`
   is the pick's tag, `pop` the population row, `wMax` its extremum (names already on the wire to the popup).
   `from` names the population member the kind's walk reads, so a caller that omits it fails rather than
   reporting a zero population; `shape` is that member's container, checked by shape and never by
   `instanceof`, because a driver may hand populations minted in its own realm. */
const CAND_KINDS = [
  // a document waiting for an instance
  { kind: "doc",  pop: "candDocs",  wMax: "candDocWMax",  from: "waiting", shape: "list" },
  // an address an application declared is a page of itself
  { kind: "seed", pop: "candSeeds", wMax: "candSeedWMax", from: "seeds",   shape: "map" },
  // a parked frontier in the cold tier
  { kind: "cold", pop: "candCold",  wMax: "candColdWMax", from: "idx",     shape: "map" },
];
/* The two container shapes a population member can be, written as what the walk calls on it. `map` requires
   `get`/`keys`/`values`/`size` together, since an array answers `keys()` with indices. */
const CAND_SHAPES = {
  list: (v) => Array.isArray(v),
  map:  (v) => v !== null && typeof v === "object" && typeof v.get === "function" &&
               typeof v.keys === "function" && typeof v.values === "function" && typeof v.size === "number",
};
/* The spread rows, readings over the served members of the ranked set, declared once because they are attached
   at the member that earns them and asserted at the record. A never-served document weighs +Infinity within its
   site, which no JSON row can carry, so it is counted in `candUnserved` (and in `candSiteUnserved` when its
   site is never served too) and folded into no spread: each spread row exists exactly when a served member was
   ranked. `candWMax`/`candWMin` are those members' site weights, `candRateMax`/`candRateMin` their own rates,
   and `candSvcMax` the most service any one of them has had.
   Each entry writes its row with a literal key, so engine/fieldgate.mjs and grep can see the writer; the
   DCHECK after the fold stops an entry naming one row and writing another. */
const CAND_SPREAD = [
  { row: "candWMax",
    fold: (c, t) => { if (!("candWMax" in c) || t.w > c.candWMax) c.candWMax = t.w; } },
  { row: "candWMin",
    fold: (c, t) => { if (!("candWMin" in c) || t.w < c.candWMin) c.candWMin = t.w; } },
  { row: "candRateMax",
    fold: (c, t) => { if (!("candRateMax" in c) || t.rate > c.candRateMax) c.candRateMax = t.rate; } },
  { row: "candRateMin",
    fold: (c, t) => { if (!("candRateMin" in c) || t.rate < c.candRateMin) c.candRateMin = t.rate; } },
  { row: "candSvcMax",
    fold: (c, t) => { if (!("candSvcMax" in c) || t.pulls > c.candSvcMax) c.candSvcMax = t.pulls; } },
];
/* The zeroed reading: the one statement of which rows a candidate walk produces. Counts start at 0 because a
   zero count is a reading; spread rows are absent because an extremum over nothing is not a number, and
   `_candRanked` attaches each on the first member. */
function _candCensus() {
  const cen = { cands: 0, candUnserved: 0, candSiteUnserved: 0, exclSub: 0, exclSeedLive: 0, exclSeedParked: 0,
                exclLive: 0, exclHeld: 0, exclStranded: 0, exclCrashed: 0, exclResting: 0 };
  for (const k of CAND_KINDS) cen[k.pop] = 0;
  return cen;
}
/* The one site that ranks a candidate: it takes the document's address, its account and the tree and returns its
   key, so a kind's total, population, extrema and the key the pick compares cannot part. Spread rows and a
   kind's `wMax` appear on its first served member. */
function _candRanked(cen, kind, url, row, tree) {
  const d = CAND_KINDS.find((k) => k.kind === kind);
  DCHECK(d !== undefined,
         "the Level-1 candidate walk ranked a work item of a kind that is not declared (`" + kind + "`) — " +
         "CAND_KINDS is what the record's accounting, the popup's columns and the admission's dispatch are " +
         "all read from, so a kind ranked outside it would be counted into the total and accounted for by no " +
         "population");
  const key = level1Key(tree, url, row, true, LEVEL1_DOOR[kind]);
  cen[d.pop]++;
  cen.cands++;
  if (key.siteW === Infinity) cen.candSiteUnserved++;
  if (key.docW === Infinity) { cen.candUnserved++; return key; }
  const t = { w: key.siteW, rate: level1Rate(row), pulls: row.pulls };
  if (!(d.wMax in cen) || t.w > cen[d.wMax]) cen[d.wMax] = t.w;
  for (const s of CAND_SPREAD) {
    s.fold(cen, t);
    DCHECK(s.row in cen,
           "a Level-1 spread row (`" + s.row + "`) was declared and its own fold did not write it — the " +
           "record asserts each spread row present exactly when a served member was ranked");
  }
  return key;
}
/* A document's identity: its address, the cookie partition key it loads under and whether its load carries the
   session, which decide which bytes and which credentials a load of it gets. Two declarations or members with
   one identity are one document; a different partition or credential state is a different one. The partition
   is enginePartitionKey's, the one this zone speaks with (so it carries engineIdentity's residual for a
   document nested in a cross-site one). `origin` is the document's principal ("null" or "null:<uuid>" for an
   opaque one). Its caller has asked the address's host. */
function documentIdentityOf(address, origin, credentialed) {
  const pk = _documentPartitionKey(address, origin, credentialed);
  return JSON.stringify([address, pk.topLevelSite, pk.hasCrossSiteAncestor, credentialed]);
}
/* The identity half of a frontier key (frontierKeyOf): the same partition key and session state, spelled
   with no `|`. */
function frontierIdentityTag(address, origin, credentialed) {
  const pk = _documentPartitionKey(address, origin, credentialed);
  return encodeURIComponent(pk.topLevelSite === null ? "" : pk.topLevelSite) + "," +
         (pk.hasCrossSiteAncestor ? 1 : 0) + "," + (credentialed ? 1 : 0);
}
function _documentPartitionKey(address, origin, credentialed) {
  DCHECK(typeof address === "string" && URL.canParse(address) && typeof origin === "string" &&
         typeof credentialed === "boolean",
         "a document identity was asked of " + JSON.stringify([address, origin, credentialed]) + " — every " +
         "member states its address, its principal and whether its load carried the session");
  const pk = enginePartitionKey(stampOrigin(origin), address);
  /* The partition key of an http(s) document of a tuple origin is the site of its own URL: the two agree
     unless the document's address is cross-site to its principal. */
  DCHECK(!_isRealOrigin(origin) || !/^https?:$/.test(new URL(address).protocol) ||
         (pk.topLevelSite === siteOf(address) && !pk.hasCrossSiteAncestor),
         "the cookie partition key of " + address + " (principal " + origin + ") is " + JSON.stringify(pk) +
         ", not the site of its own URL " + JSON.stringify(siteOf(address)));
  return pk;
}
/* Every frontier member of a population by its identity, as {kind, label}: the resident instances and the
   loading seats (each states the identity it loads), the waiting documents and the parked rows. The first
   member of an identity is the one named. */
function _frontierMembers(pop) {
  const m = new Map();
  const add = (id, kind, label) => { if (!m.has(id)) m.set(id, { kind, label }); };
  for (const e of pop.resident) {
    if (e.state === "loading") { add(e.identity, "resident", "a loading " + e.what.kind + " seat"); continue; }
    add(documentIdentityOf(e.msg.sourceUrl, e.origin, e.msg.credentialed), "resident",
        "a " + e.state + " instance at " + e.msg.sourceUrl);
  }
  for (const job of pop.waiting)
    add(documentIdentityOf(job.msg.sourceUrl, job.msg.origin, job.msg.credentialed), "waiting",
        "a waiting document at " + job.msg.sourceUrl);
  for (const row of pop.idx.values())
    add(documentIdentityOf(row.sourceUrl, row.origin, row.credentialed), "cold", "the parked entry " + row.key);
  return m;
}
/* The population the order is taken over, composed in one place and handed in, so the Level-1 order (which
   exists only in this zone) can be asked over a population a driver supplies. It is a composer, not accessors:
   what is under test is the walk, so the privates stay private, and both production call sites take the same
   population. */
function _candPopulation(idx) {
  return { resident: _pool, waiting: _waiting, seeds: _seeds, idx: idx };
}
/* The population's contract, checked against the kind table before the walk: a member the walk is not handed
   is indistinguishable downstream from an empty one (see CAND_KINDS' `from`). */
function _candPopulationCheck(pop) {
  DCHECK(pop !== null && typeof pop === "object",
         "the Level-1 candidate order was asked over no population at all — it ranks the work items that have " +
         "no instance, and the set of them is an input rather than something this walk knows");
  /* The resident set is not a kind: it is what every kind is excluded by, so a missing one would widen the
     order (every excluded item becomes a candidate) rather than empty it. */
  DCHECK(CAND_SHAPES.list(pop.resident),
         "the Level-1 candidate order was asked with no resident set — it is what a candidate is excluded BY, " +
         "so without it a parked frontier whose page a tab already holds is rehydrated beside that tab and " +
         "`exclLive` reports that nothing left the order");
  for (const k of CAND_KINDS)
    DCHECK(CAND_SHAPES[k.shape](pop[k.from]),
           "the Level-1 candidate order was asked without the `" + k.from + "` half of its population, or " +
           "with one that is not a " + k.shape + " — the `" + k.kind + "` arm walks it, so what reaches the " +
           "census is `" + k.pop + ": 0`, which is a READING (nothing of this kind was waiting) and not the " +
           "absence it actually is: a whole kind of work item leaves the order and the record agrees with " +
           "itself about a walk that never looked");
}
/* The pick is synchronous, which the index makes possible: a cluster can be rooted by a concurrent service
   round (hostNotice's create arm), so a pick acted on after a suspension could reach engineCreate's
   one-instance-per-cluster assert with a cluster that just acquired one. The caller awaits frontierIndex() once
   and hands the map in. */
/* It answers the pick and the reading together, because a `max` cannot say what it was taken over: a frozen
   rank or a row dropped from the order is visible only in the spread (`candWMax` against `candWMin`) and the
   exclusions (`exclLive`). Counts are present whenever the walk ran; weights only over a non-empty population,
   the same rule solver/result.c's `_wfq` states for `members`. */
function _bestCandidate(pop) {
  _candPopulationCheck(pop);
  const { resident, waiting, seeds, idx } = pop;
  const live = new Set();
  for (const e of resident) if (e.msg && e.msg.sourceUrl) live.add(e.msg.sourceUrl);
  /* What the store already knows about the waiting addresses, which is the rank a waiting document is
     entitled to. The key's bundle half is the engine's, so the host cannot know which residue a visit resumes,
     but every row carries its `sourceUrl`: "this address has been served C units of work for F of novel value" is
     a fact about the address being admitted, so a document cycle (A names B, B names A) sinks by its rate
     instead of re-entering as never served. Nothing refuses a second fetch; the order only prefers other work
     first. One pass over the index, only for the addresses in question. */
  const waitingAddrs = new Set();
  for (const job of waiting)
    if (!(job.msg.frameId && _isRealOrigin(job.msg.origin))) waitingAddrs.add(job.msg.sourceUrl);
  /* A declared route is ranked by its address's account too. */
  for (const seed of seeds.values()) waitingAddrs.add(seed.url);
  const byAddress = new Map();
  if (waitingAddrs.size) for (const row of idx.values()) {
    if (!waitingAddrs.has(row.sourceUrl)) continue;
    const a = byAddress.get(row.sourceUrl);
    /* Summed across the bundles served at one address: one account for the address, value over service. */
    byAddress.set(row.sourceUrl, level1Sum(a === undefined ? LEVEL1_UNSERVED : a, row, level1ProductSize()));
  }
  let best = null;
  const tree = level1Tree(resident, idx);
  /* The reading is accumulated by the walk that picks: each `w` is counted once where it is computed. */
  const cen = _candCensus();
  for (const job of waiting) {
    /* A sub-frame never roots a cluster (its embedder names it, see admit), so it is not a candidate; it keeps
       its place in `_waiting`. It is counted, so "every waiting document is a sub-frame" is told from "the
       order found nothing". */
    if (job.msg.frameId && _isRealOrigin(job.msg.origin)) { live.add(job.msg.sourceUrl); cen.exclSub++; continue; }
    live.add(job.msg.sourceUrl);
    /* An address with no rows has never been served, which is what `LEVEL1_UNSERVED` states. */
    const known = byAddress.get(job.msg.sourceUrl);
    const key = _candRanked(cen, "doc", job.msg.sourceUrl, known !== undefined ? known : LEVEL1_UNSERVED, tree);
    if (!best || level1Ahead(key, best.key)) best = { kind: "doc", job, key };
  }
  /* Declared routes: the third kind, ranked by the same weight over the same index. A declaration whose
     document a member already is (documentIdentityOf) leaves the order, counted by the member's kind, so "no
     route declared" and "every route already a member" differ; `admit` consumes it. */
  const members = seeds.size ? _frontierMembers(pop) : null;
  for (const [id, seed] of seeds) {
    const holder = members.get(id);
    if (holder !== undefined) { if (holder.kind === "cold") cen.exclSeedParked++; else cen.exclSeedLive++; continue; }
    /* What is left has never been served: no member holds its document. */
    const key = _candRanked(cen, "seed", seed.url, LEVEL1_UNSERVED, tree);
    if (!best || level1Ahead(key, best.key)) best = { kind: "seed", id, addr: seed.url, key };
  }
  /* The egress policy, read once per walk and only if a resting row asks. */
  let policyNow = null;
  for (const row of idx.values()) {
    if (live.has(row.sourceUrl)) {
      /* The exclusion moves the row's weight, it does not delete it: a parked entry and the tab holding its
         address are one work item (the tab's engine resumes the residue), so a waiting document at that address
         must have been ranked by the row's history. A row excluded because a live instance holds its address is
         carried by that engine's `engineKey` instead (its stored account plus its own service). */
      DCHECK(!waitingAddrs.has(row.sourceUrl) || byAddress.has(row.sourceUrl),
             "a parked residue was taken out of the Level-1 order because a waiting document holds its " +
             "address, and that document was not ranked by it — the row's weight has left the order with " +
             "nothing carrying it, so a productive frontier is admitted as if it had never found anything");
      cen.exclLive++;
      continue;
    }
    if (resident.some((p) => p.fkey === row.key)) { cen.exclHeld++; continue; }
    /* A stranded residue has no bytes to build an instance over, so it is not a candidate; its recipes remain
       and the next visit resumes them through engineRoot's frontierGet. */
    if (row.stranded) { cen.exclStranded++; continue; }
    /* A residue whose resume aborted this engine is the finding, recorded on the entry; re-running it unchanged
       would abort the same program at the same place. A new engine is a change, so the row returns with it. */
    if (row.crashed) { cen.exclCrashed++; continue; }
    /* A residue every member of which rests on a refused request has no move until the policy or the engine
       changes, so re-running it re-sends the page's own requests to arrive at the same rest. */
    if (row.rest !== null) {
      if (policyNow === null) policyNow = egressPolicyNow();
      if (frontierRestHolds(row.rest, policyNow)) { cen.exclResting++; continue; }
    }
    const key = _candRanked(cen, "cold", row.sourceUrl, row, tree);
    if (!best || level1Ahead(key, best.key)) best = { kind: "cold", row, key };
  }
  /* `candDocWMax`, `candSeedWMax` and `candColdWMax` stay apart because the Level-1 question is which kind is
     worth the next instance, and one merged extremum erases that comparison. */
  /* A walk that ranked members and picked nothing would be the order silently declining an admissible item. */
  DCHECK((cen.cands > 0) === (best !== null),
         "the Level-1 candidate order ranked " + cen.cands + " item(s) and picked " +
         (best ? "one" : "none") + " — the census and the pick are produced by one walk over one set, so a " +
         "disagreement is an arm that counted a candidate without offering it (or offered one without " +
         "counting it), and either way the order this zone reports is not the order it took");
  return { best: best, census: cen };
}

/* The navigation response's header list in the form that crosses the ABI: HTTP field lines `name: value`, one
   per line. A relay, not logic: every decision made from it (policy container, sandboxing flags, agent
   cluster) is the engine's. Nothing is defaulted; every producer reaching engineCreate writes `h`
   (navigationLoad's reply, a child notice's `{}`, a cold entry's stored list), and `{}` already states "no
   headers". A value with CR or LF did not come off a response (`Headers` forbids both) and is asserted here,
   where the header's name survives. */
function responseFieldLines(h) {
  DCHECK(h && typeof h === "object",
         "an engine was started with no response header list — HTML §7.5.1 creates a Document from its " +
         "response's headers, and a missing list is a producer that stopped writing one rather than a " +
         "response that carried none, which is the empty object");
  const out = [];
  for (const name of Object.keys(h)) {
    const value = h[name];
    DCHECK(typeof value === "string",
           "a response header value that is not a string reached the engine boundary (" + name + ") — a " +
           "header list holds byte strings, and anything else is a producer writing a shape this relay " +
           "would stringify into a header the server never sent");
    DCHECK(value.indexOf("\n") < 0 && value.indexOf("\r") < 0,
           "a response header value carries CR or LF (" + name + ") — the browser's own Headers forbids both, " +
           "so this value did not come off a response, and splitting it into field lines would present the " +
           "engine with headers nobody delivered");
    out.push(name + ": " + value);
  }
  return out.join("\n");
}

/* ─── Does this document load carry the person's session? ────────────────────────────────────────────
   One answer, read by every load in this file. A real navigable sends cookies: a same-origin navigation
   carries them exactly as a browser's does, so the engine is served what the person is served.
   The condition is Fetch's: a response is readable as "basic" when the request's current URL's origin is same
   origin with the request's origin (Fetch §4.1 Main fetch); otherwise a credentialed read needs §4.10 CORS
   check, which no document server grants. Asking cross-origin with cookies would spend the session for bytes
   the chokepoint must refuse, so this declines to make that request; policy remains safe-fetch.js's.
   The principal is the browser's, never re-derived from the address: `_isRealOrigin` (safe-fetch.js's own
   predicate, shared) makes an opaque principal's load uncredentialed, and an empty principal is a document
   with no session to carry.
   Named residual: a cross-origin `<iframe src>` navigation carries no cookies here, though a browser sends
   them. Next diff builds a document load type in safe-fetch.js whose read principal is the response's own
   origin (the bytes go to a different instance, not the initiator). Absence shows as cross-origin child
   documents loading in their logged-out form. */
/* A speaker is who a document load's request speaks as, {identity, referrer, policy, request}: `identity` is
   the request's client (lib/request-identity.js), `referrer` is "client" when the request's referrer is that
   client's URL and "none" when no referrer is known, `policy` is the client's policy container's referrer
   policy as documentReferrerPolicy states it, or null where this zone holds none, and `request` is the request's
   own facts as the client composed them ({destination, referrerPolicy}), or null where none are known. Each
   `speakerOf` below resolves one, or null when the browser's document it names has gone.
   documentSpeaker: a load the document `msg` describes makes for a child navigable, whose client is that
   document: the browser's frame (frameIdentity, by its id and its tab, which is its group) when the browser
   holds it, and engineIdentity for one only this zone holds (a peer-announced child, a rehydrated recipe). */
function documentSpeaker(msg, docKey) {
  return async () => {
    const live = typeof msg.documentId === "string" && typeof msg.groupId === "number";
    const identity = live ? await frameIdentity(msg.groupId, msg.documentId)
      : await engineIdentity(msg.origin, msg.sourceUrl, docKey);
    if (identity === null) return null;
    return { identity, referrer: "client", policy: live ? await documentReferrerPolicy(msg.documentId) : null,
             request: null };
  };
}

/* HTML §7.1.7 Policy containers' referrer policy item of a document's container, by the document's name in the
   engine (a live seed's browser documentId, or the engine's name for a cross-origin child this zone provisions
   off a `navigable.create` notice), as its installing instance states it (`GetReferrerPolicy`): Referrer Policy
   §8.1 over the response, then HTML §4.2.5.1's meta `referrer` for each `<meta>` the parse inserted. Each entry
   is {promise, settle}, opened by the seed's AST_ANALYZE or by the create arm, and settled once: with the policy
   by engineRoot or engineJoin, or with null where those bytes are installed nowhere (no document delivered, a
   sub-frame seed whose Document its embedder's instance builds, a cleared or failed boot). Entries live as long
   as the offscreen session. */
const _documentReferrerPolicies = new Map();

/* Opens `documentId`'s entry; a re-delivery of the same document keeps the first dispatch's. */
function documentPolicyOpen(documentId) {
  if (_documentReferrerPolicies.has(documentId)) return;
  let settle = null;
  const promise = new Promise((resolve) => { settle = resolve; });
  _documentReferrerPolicies.set(documentId, { promise, settle });
}

/* Settles `documentId`'s entry with `policy` (a Referrer Policy §3 policy, "" for none stated, or null for no
   installed document); the first settle decides it. */
function documentPolicySettle(documentId, policy) {
  const e = _documentReferrerPolicies.get(documentId);
  DCHECK(e !== undefined, "a document's referrer policy was settled for a name no AST_ANALYZE or create arm " +
                          "opened (" + documentId + ")");
  DCHECK(policy === null || policy === "" || REFERRER_POLICIES.indexOf(policy) >= 0,
         "a seed's referrer policy was settled as " + JSON.stringify(policy) + ", which is not a Referrer Policy " +
         "§3 policy — enginePolicySettle refuses an instance's other answers and settles null for them");
  e.settle(policy);
}

/* The referrer policy, "" included, of `documentId`'s container as its installing instance states it; null
   when no instance installed that seed's bytes. */
async function documentReferrerPolicy(documentId) {
  const e = _documentReferrerPolicies.get(documentId);
  return e === undefined ? null : await e.promise;
}

/* seedSpeaker: the seed of the live document `msg`, re-loading what the browser's navigation loaded. A
   sub-frame's navigation had its parent document as client, found by the browser's `parentDocumentId` and named
   in the engine by documentNameOf, and its request is the one the parent's instance created the child with
   (seedChildRequest waits for that create): HTML §7.4.5's destination and the request referrer policy after
   Fetch §4.1 Main fetch. Where no instance names the parent, or it left the pool without creating the child,
   the load states DOCUMENT_REQUEST_UNSTATED. A top-level navigation's initiator is not recorded, so the
   document speaks as itself, with no referrer.
   Named residual. Not covered: a top-level document's initiator, and a frame the parent's instance never
   creates (a container not built, a branch not reached), whose seed waits for that instance to leave the pool.
   The next diff gives the engine the containers it does not build. Their absence shows as a seed load's
   Referer, Sec-Fetch-Site and Sec-Fetch-Dest differing from the browser's navigation of that document, and as
   a seed not loaded while its parent's instance runs. */
function seedSpeaker(msg) {
  return async () => {
    const frames = await swRpc("webNavigation.getAllFrames", { tabId: msg.groupId });
    if (frames === null) return null;
    const f = frames.find((x) => x.documentId === msg.documentId);
    if (f === undefined) return null;
    if (f.parentFrameId === -1) {
      const own = await frameIdentityOf(f);
      return own === null ? null : { identity: own, referrer: "none", policy: null, request: null };
    }
    DCHECK(typeof f.parentDocumentId === "string" && f.parentDocumentId !== "",
           "webNavigation reported a sub-frame with no parentDocumentId — the seed load cannot name its client");
    const parent = await frameIdentity(msg.groupId, f.parentDocumentId);
    if (parent === null) return null;
    const parentName = await documentNameOf(frames, f.parentDocumentId);
    /* A parent the engine built inside its embedder's instance has no policy entry; the request facts carry
       its container's policy already (Fetch §4.1 Main fetch's substitution, at the create). */
    const policy = parentName === null ? null : await documentReferrerPolicy(parentName);
    return { identity: parent, referrer: "client", policy,
             request: parentName === null ? null : await seedChildRequest(parentName, msg.seedUrl) };
  };
}

/* HTML §7.4.5's request facts {destination, referrerPolicy} of each child navigable an instance created, by
   creating document name and address, from the engine's `navigable.request` notices (core/frame/navigable.c's
   create), with the address after redirects added once this zone has loaded a cross-origin child. Each creator's
   record is {facts, waiters, names, nameWaiters, closed, instance}: `closed` once the instance that held the
   creator has left the pool, after which no notice for it can arrive, and `instance` the instance holding a
   same-origin realm the pool names nowhere. Records live as long as the offscreen session. */
const _childRequests = new Map();

function childRequestsOf(creator) {
  let c = _childRequests.get(creator);
  if (c === undefined) {
    c = { facts: new Map(), waiters: new Map(), names: new Map(), nameWaiters: new Map(), closed: false,
          instance: null };
    _childRequests.set(creator, c);
  }
  return c;
}

/* Records `facts` for (creator, address); the first record of a pair stands. Answers any seed waiting on it. */
function childRequestRecord(creator, address, facts) {
  DCHECK(NAVIGATION_DESTINATIONS.indexOf(facts.destination) >= 0 &&
         (facts.referrerPolicy === "" || REFERRER_POLICIES.indexOf(facts.referrerPolicy) >= 0),
         "a child navigable's request facts " + JSON.stringify(facts) + " are not a Fetch §2.2.5 navigation " +
         "destination and a Referrer Policy §3 policy — core/frame/navigable.c composes both");
  const c = childRequestsOf(creator);
  if (!c.facts.has(address)) c.facts.set(address, facts);
  const w = c.waiters.get(address);
  if (w !== undefined) { c.waiters.delete(address); for (const resolve of w) resolve(c.facts.get(address)); }
}

/* A child load of `named` by `creator` landed on `landed`, the address the browser's own sub-frame reports after
   redirects, so that address answers with the facts the create named. A load no create announced (a later
   navigation of the frame) has none to file. */
function childRequestAlias(creator, named, landed) {
  const c = childRequestsOf(creator);
  const n = new URL(named).href;
  if (!URL.canParse(landed)) return;
  if (c.facts.has(n)) childRequestRecord(creator, new URL(landed).href, c.facts.get(n));
  if (c.names.has(n)) childNameRecord(creator, new URL(landed).href, c.names.get(n));
}

/* The engine's name `child` of the child navigable's document `creator` created at `address` (the
   `navigable.request` notice's, or a landed address childRequestAlias files); the first record stands, and any
   lookup waiting on that address is answered. */
function childNameRecord(creator, address, child) {
  DCHECK(typeof child === "string" && child !== "", "a child navigable's name was recorded empty");
  const c = childRequestsOf(creator);
  if (!c.names.has(address)) c.names.set(address, child);
  const w = c.nameWaiters.get(address);
  if (w !== undefined) { c.nameWaiters.delete(address); for (const resolve of w) resolve(c.names.get(address)); }
}

/* The engine's name of the child `creator` created at `url`, waiting for its `navigable.request` notice while the
   instance holding `creator` can still send one; null once that instance has left the pool. */
async function seedChildName(creator, url) {
  const c = childRequestsOf(creator);
  const address = new URL(url).href;
  if (c.names.has(address)) return c.names.get(address);
  if (!childRequestsLive(creator)) return null;
  return await new Promise((resolve) => {
    if (!c.nameWaiters.has(address)) c.nameWaiters.set(address, []);
    c.nameWaiters.get(address).push(resolve);
  });
}

/* The engine's name of the browser's document `documentId` among `frames` (webNavigation.getAllFrames' answer):
   its own documentId where an instance installed its seed, otherwise the name its parent's instance created it
   under (the `navigable.request` notice's child), found by the parent's name and the frame's address. Null for
   a top-level document no instance installed. A document's entry is opened here when its own AST_ANALYZE has
   not arrived yet, so a descendant's seed waits for that dispatch to install or settle it.
   Named residual. Not covered: a child whose navigable's initial `about:blank` was read before its load, whose
   loaded document the engine names afresh (navigable.c's load mints from the navigable's name). The next diff
   announces that name on the load. Its absence shows as a seed nested in such a frame waiting for its parent's
   instance to leave the pool. */
async function documentNameOf(frames, documentId) {
  documentPolicyOpen(documentId);
  if ((await documentReferrerPolicy(documentId)) !== null) return documentId;
  const f = frames.find((x) => x.documentId === documentId);
  if (f === undefined || f.parentFrameId === -1 || !URL.canParse(f.url)) return null;
  const parent = await documentNameOf(frames, f.parentDocumentId);
  return parent === null ? null : await seedChildName(parent, f.url);
}

/* Whether document `name` can still create a child: its instance, named in the pool or, for a same-origin realm,
   the one that announced it, is still in the pool and has not been closed. */
function childRequestsLive(name) {
  const c = childRequestsOf(name);
  if (c.closed) return false;
  return hostHolderOf(name) !== null || (c.instance !== null && _pool.indexOf(c.instance) >= 0);
}

/* The instance `eng` has left the pool: every document it held, its same-origin realms included, creates no more
   children, so their waiting seeds are answered null. */
function childRequestsClose(eng) {
  /* A loading seat holds no document yet. */
  if (eng.docId === null) {
    DCHECK(eng.state === "loading", "an instance with no document name left the pool in state " + eng.state);
    return;
  }
  const realms = [];
  for (const [name, r] of _childRequests) if (r.instance === eng) realms.push(name);
  for (const name of [eng.docId].concat(eng.joinedDocIds, realms)) {
    const c = childRequestsOf(name);
    c.closed = true;
    for (const w of c.waiters.values()) for (const resolve of w) resolve(null);
    for (const w of c.nameWaiters.values()) for (const resolve of w) resolve(null);
    c.waiters.clear();
    c.nameWaiters.clear();
  }
}

/* The request facts the instance holding `parentDocumentId` created its child at `url` with, waiting for its
   `navigable.request` notice while that instance can still create one; null once it has left the pool without
   creating that child, or when no instance holds the parent. */
async function seedChildRequest(parentDocumentId, url) {
  const c = childRequestsOf(parentDocumentId);
  const address = new URL(url).href;
  if (c.facts.has(address)) return c.facts.get(address);
  if (!childRequestsLive(parentDocumentId)) return null;
  return await new Promise((resolve) => {
    if (!c.waiters.has(address)) c.waiters.set(address, []);
    c.waiters.get(address).push(resolve);
  });
}

/* Why a seed's load `loaded` is not this document, in the page-source vocabulary, or null when it is;
   `asked` is the origin of the address the browser reported. Both rules sit on top of the loader's. An OK
   empty body is an ordinary empty Document (HTML §7.4.5), but a seed with no bytes cannot be the program this
   run explores, and analysing it would read as a clean page. A load that landed on another origin is not this
   document: the Document's URL is the response's (HTML §7.4.5) while the principal is the browser's
   MessageSender.origin, so it would seat a document of origin B in a cluster keyed on A (engineJoin's assert
   and SECURITY.md's one-instance-per-origin-keyed-agent-cluster forbid that). safeFetch already refuses a
   credentialed load that redirects off-origin (`blocked-cors-credentialed:<origin>`); this catches the
   uncredentialed case. It is a report, not an assert: the server chose the redirect, and a second GET of the
   address the person's own navigation landed on arriving elsewhere is evidence of a single-use token or bot
   challenge. */
function seedUnavailable(loaded, asked) {
  const landed = originOf(loaded.url);
  return loaded.unavailable !== null ? loaded.unavailable
       : (landed === "" || landed !== asked)
         ? { kind: "network", detail: "cross-origin-redirect:" + (landed || "unparseable") }
       : loaded.bytes.length === 0 ? { kind: "empty" } : null;
}

function navigationCarriesSession(absUrl, principalOrigin) {
  DCHECK(typeof principalOrigin === "string",
         "a document load was asked whether it carries the session with no principal stated at all — every " +
         "producer writes a string (the browser's MessageSender.origin for a live document, the origin of " +
         "the URL this zone fetched for a peer's child, the parked one for a cold recipe), so `undefined` " +
         "here is a producer that stopped writing the field rather than a document with no session");
  return _isRealOrigin(principalOrigin) && originOf(absUrl) === principalOrigin;
}

/* The facts a credentialed document load of `absUrl` speaks with as `speaker`: HTML §7.4.5's request, credentials
   mode `include` and mode `navigate`, with the speaker's client as the request's, so its Cookie, Referer and
   Sec-Fetch-Site are that client's. The document that reads the reply is the caller's principal, a second fact.
   `request` is the request's own facts as its composer states them (DOCUMENT_REQUEST_UNSTATED or a
   `document.fetch` record): where it states a referrer policy, Fetch §4.1 Main fetch has already put the policy
   container's in place of "", so the speaker's policy is not read. */
async function navigationIdentity(speaker, absUrl, request) {
  DCHECK(!!speaker && !!speaker.identity && typeof speaker.identity.origin === "string" &&
         !!speaker.identity.partitionKey && (speaker.referrer === "client" || speaker.referrer === "none"),
         "a credentialed document load was handed no {identity, referrer} to speak as");
  DCHECK(speaker.request === null || typeof speaker.request === "object",
         "a document load's speaker states no `request` — every speaker states its client's request facts or null");
  DCHECK(speaker.policy === null || speaker.policy === "" || REFERRER_POLICIES.indexOf(speaker.policy) >= 0,
         "a document load's speaker states the client policy " + JSON.stringify(speaker.policy) +
         ", which is neither null (none held) nor a Referrer Policy §3 policy");
  /* Referrer Policy §3.9 The empty string: a container stating none falls back to the default referrer policy. */
  const policy = request.referrerPolicy !== null || speaker.policy === null || speaker.policy === ""
    ? "strict-origin-when-cross-origin" : speaker.policy;
  return identityFor(speaker.identity, { url: absUrl, requestInit: { credentials: "include", mode: "navigate",
                                         destination: request.destination,
                                         referrer: speaker.referrer === "client" ? null : "",
                                         referrerPolicy: request.referrerPolicy } },
                     policy);
}

/* A document load's own request facts, {destination, referrerPolicy}, for a composer that states neither: Fetch
   §2.2.5's `document`, and no referrer policy, so the speaker's client policy applies. A speaker whose client
   knows the request (seedSpeaker's `request`) states it in place of this.
   Named residual. Not covered: a peer-provisioning notice's destination and request referrer policy, and a
   seed's where seedSpeaker states none. The `navigable.create` and `navigable.swap` notices do not carry the
   facts the `document.fetch` record does. The next diff writes the two fields onto those notices from the same
   create. Their absence shows as Sec-Fetch-Dest `document` where the browser's own load of that frame sent the
   container's kind. */
const DOCUMENT_REQUEST_UNSTATED = Object.freeze({ destination: "document", referrerPolicy: null });

/* Fetch §2.2.5 Requests' navigation request destinations, the values a document load's request may state. */
const NAVIGATION_DESTINATIONS = Object.freeze(["document", "embed", "frame", "iframe", "object"]);

/* ─── The one document-load path ────────────────────────────────────────────────────────────────────
   HTML §7.4 Navigation and session history's load, as this host performs it: an address goes in and a §7.4.5
   Populating a session history entry response comes out (the bytes a Document is parsed from, the header list
   its policy container is built from, the URL its origin is determined over). Every document the engine holds
   arrives here, through lib/safe-fetch.js, so scheme allowlist, SSRF/PNA guard on the initial and
   post-redirect URL, CORB and the destructive-path deny list apply to every navigation. It carries the
   session where a browser's navigation would (see navigationCarriesSession).
   Two paired outcomes, never defaulted: `bytes` is a byte sequence and `unavailable` null, or `bytes` is null
   and `unavailable` names why in the closed vocabulary the popup renders (status/empty/network). `bytes: null`
   is a load that did not load: the navigable exists and shows an error page. A third outcome, `declined`, is
   below. The chokepoint's refusals arrive as `kind: "network"` with the rule in `statusText`
   (`blocked-signal:`, `blocked-scheme:`, `blocked-local-network:`).
   Emptiness is not judged here: an OK zero-length body is an ordinary empty Document; refusing one is a seed's
   rule, stated at the seed. */
/* `fromDoc` is this zone's record of the document issuing this navigation (lib/document-record.js), distinct
   from `provenance` (the engine's word about this navigation act); safe-fetch.js reads both, the record as a
   floor. An omitted one is refused by `_docOf`. The produced document's record is the join of the two
   (childDocRecord), composed by whoever states that document's analyze record. */
/* What a caller of a §7.4 load does with a refusal, as a declared word rather than a boolean, because "holds a
   rendezvous" and "can carry a refusal" differ at the route seed:
     `rendezvous` — the engine is parked on a request id; the refusal is routed to it (engineDeliverDocument →
                    HostDecline → engine_host_decline) and the navigable keeps the pre-operation document HTML
                    §7.3.1.3 Child navigables' create-a-new-child-navigable created it holding.
     `report`     — nothing is provisioned or parked; the caller drops its work item and reports the
                    chokepoint's reason verbatim (a route seed, via admitSeatLand's seat-drop arm).
     `none`       — the caller has no arm that can state a load that was never attempted, so a decline aborts
                    and the message names what each such caller would fabricate.
   The test is written in the positive (`!== "rendezvous" && !== "report"`), so an unstated or misspelled word
   refuses by construction. */
const DOC_REFUSAL_ARMS = Object.freeze(["rendezvous", "report", "none"]);
/* Whether a §7.4 load of `abs` read by an opaque (`readerOpaque`) or principal-`principalOrigin` document asks
   its speaker for the session: never for an opaque reader (an opaque origin is same-origin with nothing). */
function navigationCredentialed(abs, principalOrigin, readerOpaque) {
  return !readerOpaque && navigationCarriesSession(abs, principalOrigin);
}
/* `speakerOf` resolves the speaker (documentSpeaker's, seedSpeaker's) whose session a load the browser would
   credential carries, or null when it has none, which loads without the session. Every answer states
   `credentialed`, whether this load carried the session, which the loaded document's record keeps. */
async function navigationLoad(u, base, principalOrigin, provenance, fromDoc, refusalArm,
                              actor, driven, pinned, request, speakerOf, readerOpaque) {
  DCHECK(!!request && NAVIGATION_DESTINATIONS.indexOf(request.destination) >= 0 &&
         (request.referrerPolicy === null || request.referrerPolicy === "" ||
          REFERRER_POLICIES.indexOf(request.referrerPolicy) >= 0),
         "a §7.4 navigation was loaded with request facts " + JSON.stringify(request) + " — a navigation " +
         "request's destination is one of Fetch §2.2.5's navigation destinations and its referrer policy is a " +
         "Referrer Policy §3 policy or null for none stated");
  /* `refusalArm` is required; an unstated word takes the refusing arm, exactly as `none` does. */
  DCHECK(DOC_REFUSAL_ARMS.indexOf(refusalArm) >= 0,
         "a §7.4 navigation was loaded stating the refusal arm " + JSON.stringify(refusalArm) + ", which " +
         "is none of the three this zone declares (" + DOC_REFUSAL_ARMS.join("/") + ") — the word says what " +
         "this caller DOES with a refusal, and a caller that does not state it would have this function pick " +
         "for it: a rendezvous it does not hold, a report it cannot write, or an empty document for an " +
         "address nothing ever fetched");
  /* `actor` is whose act this navigation is, stated by each caller: a child navigable is the page's, a declared
     route's load and an ambient seed are the tool's (safe-fetch.js's `_actorOf`: a request this tool composed
     at a person's direction is still `tool`). It is the signal the egress registry grades
     `certain`, shown on the person's own control, so it must be true. The value space is asked of the
     chokepoint and never copied here. */
  DCHECK(typeof self.safeFetchSignalUsable === "function" &&
         self.safeFetchSignalUsable("actor", actor) === null,
         "a \u00a77.4 navigation was loaded with an actor word the chokepoint's own registry does not " +
         "gate on: " + JSON.stringify(actor) + " — the word says whether the ANALYSED PAGE composed " +
         "this navigation or THIS TOOL did, which no other signal can answer, and safe-fetch.js aborts on an " +
         "absent one. It is stated per caller rather than once here because this loader serves a child " +
         "navigable (the page), the load of a route an application declared of itself (the tool), and an " +
         "ambient seed this tool composed from an address the person's browser navigated to (the tool)");
  /* `driven` and `pinned` are the engine's drive and witness marks off the record a navigation relays
     (`driven`/`undriven`, `pinned`/`unpinned`), and `unstated` for one that relays none (an ambient seed). A
     `page` navigation always relays one; a declared route's `tool` load relays its declaration's.
     safe-fetch.js's `_drivenOf` CHECKs the drive mark's pairing with `page`. */
  DCHECK(actor === "page" ? (pinned === "pinned" || pinned === "unpinned")
                          : (pinned === "pinned" || pinned === "unpinned" || pinned === "unstated"),
         "a \u00a77.4 navigation was loaded with the witness mark " + JSON.stringify(pinned) + " for actor `" +
         actor + "` — a page navigation relays the engine's mark off its record, and a tool one relays its " +
         "record's or states `unstated`");
  /* The requested address, resolved once because every arm owes a URL: a navigable whose load did not load
     still gets a Document, at the URL that was requested. An unparseable address is a caller's serializer
     disagreeing with the URL parser, a broken contract, so it throws (a DFAIL would leave arms reading
     `undefined` in release). */
  const abs = new URL(u, base).href;
  /* The chokepoint is not optional: ast-worker.html loads safe-fetch.js before this file, so its absence is
     that load order broken. */
  DCHECK(typeof self.safeFetch === "function",
         "a §7.4 navigation was asked for with no network chokepoint installed — lib/safe-fetch.js is loaded " +
         "before this file in ast-worker.html, so its absence is that load order broken and every document " +
         "of this session would report as a load that did not load");
  /* The private-network principal is `fromDoc`'s address space (safeFetch reads it off the record), never
     re-derived from `u`, so a requested address cannot authorize itself into the user's intranet. The
     credentialed-read principal is a second fact: `principalOrigin` is a browser-stated origin deciding whose
     authenticated bytes may be read. A sandboxed frame reports an ordinary address and an opaque origin, so
     deriving one from the other would give it its embedder's session. */
  DCHECK(typeof principalOrigin === "string",
         "a §7.4 navigation was asked for with no credentialed-read principal — it is the browser's " +
         "MessageSender.origin for the document this load belongs to, and it is what decides whether the " +
         "person's session travels; `undefined` is a caller that stopped stating it, which would silently " +
         "return every tab to loading the LOGGED-OUT document");
  /* ── Who named this address ─────────────────────────────────────────────────────────────────────────
     A top-level navigation is a GET, in RFC 9110 §9.2.1 Safe Methods' safe set, so what remains is provenance.
     The firing decision is not made here: safe-fetch.js reads the word this call passes (`_provenanceOf` is a
     CHECK, fatal in release) and `_firingRefusal` is the per-origin widening, so the one decision serves both
     hosts at the line that opens the socket. At an unconfigured origin a derived navigation is refused like a
     forced one (the default fires program loads only), and the person widens the origin to permit it. A
     refusal comes back as the chokepoint's `blocked-signal:` reply record. */
  /* A load whose reader is an opaque document carries no session: the session decision is about who reads the
     bytes, and an opaque origin is same-origin with nothing, as for a cross-origin child (SECURITY.md). */
  DCHECK(typeof readerOpaque === "boolean",
         "a §7.4 navigation was asked for without stating whether its reader is an opaque document");
  /* Fetch §4.3 "Scheme fetch"'s "data" arm. A data: URL's bytes are in the URL and its load reaches no other
     party, so it is not asked of safeFetch, whose every signal is about a party a request reaches; the realm's
     data: URL processor answers it (safeFetchSchemeData). Its document's origin is opaque (principalOf), so no
     session can travel either. HTML §7.4 states no data: step, so a top-level navigation the page initiates
     loads too, where Chrome refuses it ("Not allowed to navigate top frame to data URL") and allows a child's. */
  if (new URL(abs).protocol === "data:") {
    const d = await self.safeFetchSchemeData(abs);
    if (d.refusal)
      return { url: abs, headers: {}, bytes: null, declined: null, setCookies: [], credentialed: false,
               unavailable: { kind: "network", detail: d.statusText } };
    return { url: abs, headers: d.headers, bytes: d.body, unavailable: null, declined: null, setCookies: [],
             credentialed: false };
  }
  const speaker = navigationCredentialed(abs, principalOrigin, readerOpaque) ? await speakerOf() : null;
  const credentialed = speaker !== null;
  /* The request as the speaker's client composed it, where the speaker knows it (a sub-frame seed). */
  DCHECK(speaker === null || speaker.request === null ||
         (NAVIGATION_DESTINATIONS.indexOf(speaker.request.destination) >= 0 &&
          typeof speaker.request.referrerPolicy === "string"),
         "a document load's speaker states request facts " + JSON.stringify(speaker && speaker.request) +
         " that are neither null nor a navigation destination and a referrer policy");
  if (speaker !== null && speaker.request !== null) request = speaker.request;
  const identity = credentialed ? await navigationIdentity(speaker, abs, request) : null;
  try {
    // Never `as: "script"`: these bytes are parsed as a document, not run as code.
    /* The session is carried where a browser's navigation would carry it; the chokepoint re-decides on the
       bytes that come back (credentialed SOP over the post-redirect origin), so a load that leaves this origin
       is refused there. Asking for cookies arms the destructive-path deny list, which safe-fetch.js scopes to
       `credentialed && provenance !== "observed"`: a document whose address carries a listed token is loaded
       when the person's browser navigated to it and refused when this tool derived or forced the address. */
    /* Fetch §2.2.5 Requests' destination as the request states it: `document`, or HTML §7.4.5's container local
       name. Not script-like, so no CORB: the HTML parser reads these bytes. */
    /* `provenance` is the caller's statement of who named this address, relayed verbatim; the chokepoint owns
       the firing decision. */
    /* `credentials: "include"` is the navigation's own credentials mode (HTML §7.4.5 Populating a session
       history entry: "destination `document` … credentials mode `include`"); `credentialed` is this zone's
       willingness to spend the session, and safe-fetch.js's `_credentialedOf` composes the two. */
    /* `doc` is `fromDoc`, the record of the instance this request was made from (the initiator), not the
       join, which names the document these bytes will become. */
    /* `actor` and `provenance` are independent: a page navigates itself on a forced arm as readily as on an
       observed one. */
    /* `mode: "navigate"`: HTML §7.4.5's fetch is the navigate algorithm's, whose reader is the instance the
       document becomes, not the page that named it. */
    const r = await self.safeFetch(abs, { pageOrigin: principalOrigin,
                                          destination: request.destination, mode: "navigate",
                                          provenance: provenance,
                                          doc: fromDoc, actor: actor, driven: driven,
                                          pinned: pinned,
                                          credentials: "include", credentialed, identity });
    DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && r.headers && typeof r.headers === "object",
           "safeFetch answered a document load with something other than its reply record — HTML §7.4.5 " +
           "\"Populating a session history entry\"'s attempt-to-populate reads the BODY and the POLICY off " +
           "it, and a Document judged under no policy is how a page whose CSP kills a sink gets reported as " +
           "exploitable");
    DCHECK(typeof r.statusText === "string",
           "safeFetch answered a document load with no statusText — every refusal it makes carries its REASON " +
           "there (`blocked-scheme:`, `blocked-local-network:`, `blocked-corb:`), which is the whole of " +
           "what a seeded page that could not be loaded has to tell its reader");
    /* Fetch §2.2.6 Responses' URL list, whose last item is the response's URL. Only this zone saw the redirect
       chain, and the engine determines the Document's origin over this string: a same-origin request that
       redirects off-origin yields a Document of another agent cluster. Asserted, since safeFetch was handed an
       absolute href and an empty list is its contract changing. */
    DCHECK(Array.isArray(r.urlList) && r.urlList.length >= 1 &&
           typeof r.urlList[r.urlList.length - 1] === "string" && r.urlList[r.urlList.length - 1] !== "",
           "safeFetch answered a document load with no Fetch §2.2.6 URL LIST — its last item is the " +
           "RESPONSE's URL, which is what HTML §7.4.5 determines the loaded Document's origin, CSP §2.2.2's " +
           "self-origin and §7.5.1's creationURL over. Without it a redirect off this origin becomes a " +
           "Document created under the principal of the address that was merely requested");
    const finalUrl = r.urlList[r.urlList.length - 1];
    /* Status 0 is not a reply: the chokepoint answers it when the request never went on the wire (blocked
       scheme, private target, CORB, unparseable URL), with the reason in `statusText`, which travels. */
    /* A decline is not an error page. `bytes: null` reads in the engine's `child_document` as a navigable
       showing an error page (opaque origin), which is right where the network refused. Where this zone
       declined, no load was attempted, and HTML §7.3.1.3 Child navigables leaves the navigable at the
       `about:blank` it was created holding, which inherits the creator's origin; flows forked in that frame
       differ between the two. */
    if (r.refusal) {
      DCHECK(r.refusal.kind === "network" || r.refusal.kind === "decline",
             "safeFetch graded a §7.4 navigation's refusal `" + r.refusal.kind + "`, which is neither word it " +
             "states — and the arm an unknown grade falls to here reports the load as a page whose server " +
             "could not be reached");
      /* The decline goes to the two arms that can state it (`rendezvous`, `report`) and aborts at `none`, whose
         three callers would fabricate different things: the `navigable.create` and `navigable.swap` notices
         (no request id; they would turn a null body into an empty document) and the ambient seed (it can
         report an unavailability through `onNavigationOutcome` but has no page-source kind for a decline in
         offscreen-brain.js's `_PAGE_SOURCE_KINDS`). The DFAIL names what to build for each. */
      if (r.refusal.kind === "decline") {
        if (refusalArm !== "rendezvous" && refusalArm !== "report")
          DFAIL("a §7.4 navigation was DECLINED by the chokepoint (" + r.refusal.reason + ") for a caller " +
                "whose refusal arm is `" + String(refusalArm) + "` — it has NO arm that can state a load " +
                "that was NEVER ATTEMPTED, so anything it is handed becomes a claim that is false. Its " +
                "`unavailable` vocabulary is status/empty/network and every one of the three says a load was " +
                "ATTEMPTED and failed, so answering with any of them hands the engine a navigable showing an " +
                "error page for an address nothing ever fetched, where §7.3.1.3 \"Child navigables\" leaves " +
                "the creator-inherited `about:blank` the navigable was created holding. THREE CALLERS STATE " +
                "`none` AND THEY FABRICATE DIFFERENT THINGS, so the thing to build differs and this names " +
                "which you are standing in. (1) A `navigable.create` NOTICE and (2) a `navigable.swap` " +
                "notice: peer-provisioning records with no request id, whose own body lines turn a null " +
                "body into an EMPTY Uint8Array — so what has to be built is a refusal the NOTICE channel " +
                "can carry, or the creating engine's own park reached some other way. (3) The AMBIENT SEED: " +
                "it CAN report an unavailability (`onNavigationOutcome`) and it CANNOT report a decline, " +
                "because offscreen-brain.js's `_PAGE_SOURCE_KINDS` is `status`/`empty`/`network` and every " +
                "one of those says a load was attempted — so what has to be built is a FOURTH page-source " +
                "kind, through that list, its sibling asserts and the popup that renders it. A ROUTE SEED " +
                "USED TO BE ON THIS LIST AND IS NOT: it states `report`, and `admitSeatLand`'s seat-drop arm " +
                "is what answers it. a3a93fd widened the old boolean to all three forward consumers with a " +
                "literal, which turned a refused child navigable into a silently empty one; fetchedDocument " +
                "takes the word from its consumer now");
        /* The third outcome: `bytes` and `unavailable` are both null, and `declined` is the reason, the only
           field telling "no load attempted" (creator-origin `about:blank`) from "a load failed" (opaque error
           page). */
        return { url: finalUrl, headers: {}, bytes: null, unavailable: null, declined: r.refusal.reason,
                 setCookies: [], credentialed };
      }
    }
    if (r.status === 0)
      return { url: finalUrl, headers: {}, bytes: null, declined: null, setCookies: [], credentialed,
               unavailable: { kind: "network", detail: r.statusText } };
    /* The response's credentialed Set-Cookie fields, each with the URL it answered, for the asking flow's own
       jar: stored during the fetch (Fetch §4.7), so an error response's count too. */
    DCHECK(Array.isArray(r.setCookies), "safeFetch answered a document load with no Set-Cookie list");
    /* Not OK is a load that did not load: the navigable shows an error page at the URL the response came from.
       An error response's body is never used as the document. */
    if (!r.ok)
      return { url: finalUrl, headers: {}, bytes: null, declined: null, setCookies: r.setCookies, credentialed,
               unavailable: { kind: "status", status: r.status } };
    /* The bytes, because a Document is parsed from a byte sequence: HTML §13.2.3.2 Determining the character
       encoding is the engine's algorithm. And the whole header list: which names matter (opener policy,
       embedder policy, `Origin-Agent-Cluster`) is the engine's question. */
    return { url: finalUrl, headers: r.headers, bytes: r.body, unavailable: null, declined: null,
             setCookies: r.setCookies, credentialed };
  } catch (e) {
    /* A thrown fetch is a network error (Fetch §5.6 Fetch methods) and a real outcome; an invariant abort
       thrown through this catch is not, so it is rethrown. */
    RETHROW_FATAL(e);
    return { url: abs, headers: {}, bytes: null, declined: null, setCookies: [], credentialed,
             unavailable: { kind: "network", detail: String((e && e.message) || e) } };
  }
}

/* The embedder policy item (HTML §7.1.4 Cross-origin embedder policies) of §7.1.7's policy container, as this
   zone relays it beside the CSP list. This zone does not interpret it: only the engine maps the tokens back
   to values, and it crashes on an unknown one. It owes only that the four items arrive unchanged and whole. A
   caller with no creator states NEW_EMBEDDER_POLICY, the section's initial value, since every container has
   one and there is no empty spelling. */
const NEW_EMBEDDER_POLICY = Object.freeze({ value: "unsafe-none", endpoint: "",
                                            reportOnlyValue: "unsafe-none", reportOnlyEndpoint: "" });
/* The referrer policy item of that same container (HTML §7.1.7: "It is initially the default referrer
   policy"), which a caller with no creator states. It is not "", because "" is a policy value of its own
   (Referrer Policy §3.9 "The empty string"). */
const DEFAULT_REFERRER_POLICY = "strict-origin-when-cross-origin";

/* Whole or not at all, like the CSP list's two halves: §7.1.7's clone moves every item, so half an item
   arrives having replaced the creator's answer with the default. */
function embedderPolicyWhole(e) {
  return !!e && typeof e.value === "string" && e.value !== "" && typeof e.endpoint === "string" &&
         typeof e.reportOnlyValue === "string" && e.reportOnlyValue !== "" &&
         typeof e.reportOnlyEndpoint === "string";
}

// ---- Engine lifecycle over one wasm instance (one agent cluster) ----
/* `docName` is set only for a document another engine created: its name arrived in that engine's
   navigable.create notice (HTML §4.8.5 The iframe element creates a child navigable inside the insertion
   steps and cannot ask this zone). A root document is named here, by the counter above. */
/* `topLevelUrl` is the top-level creation URL of HTML §8.1.3.1 Environments. A root document is its own
   top-level traversable; a document a peer engine created carries its creator's decision on the notice. It is
   separate from the address because this document may be nested in another instance's document, and HTML
   §8.1.3.5 Secure contexts decides from the top of that chain. */
/* Provision the instance, then root it. The renderer is a frame in this document and is never collected, so
   anything that throws after it exists (init's return code, the bundle id, the key's origin, an IndexedDB
   read, the engine's abort) must take the frame with it; separating the two operations makes that cleanup
   one line. The renderer's name is this instance's agent cluster key: renderer-host.js takes a name and
   computes nothing, so the name this zone passes is the answer, and it identifies the frame. */
function engineCreate(code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable,
                      openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags, referenced) {
  /* The one way to obtain an instance, asserted rather than met as a TypeError inside admit's catch (which
     would report a boot abort of an instance never built). render-process-host.js holds which agent clusters
     have an instance, mints the routing id and refuses a second for one cluster in every build;
     renderer-host.js materializes the frame for the id it is given. */
  DCHECK(typeof self.rendererLaunch === "function",
         "renderer-host.js is not loaded in this zone — it is what obtains an instance once the registry has " +
         "admitted its agent cluster, so without it every document would be reported as a crashed instance " +
         "rather than as a bridge that is missing half of itself");
  /* The clone of the creator's policy container (HTML §7.1.7 Policy containers), or `null` for a document with
     no creator (a reported root, a rehydrated recipe, a group swap). Stated by every call site and never
     defaulted: a create notice always carries one, so `null` and "the caller forgot" must stay distinct. */
  DCHECK(inherited === null ||
         (!!inherited && typeof inherited.csp === "string" && typeof inherited.cspReportOnly === "string" &&
          typeof inherited.selfOrigin === "string" &&
          inherited.selfOrigin !== "" && embedderPolicyWhole(inherited.embedder) &&
          _REFERRER_POLICIES.includes(inherited.referrerPolicy)),
         "an instance was started with an inherited policy container that is neither `null` nor a WHOLE one — " +
         "CSP §2.2 makes a list a struct of policies AND a self-origin, and HTML §7.1.7 makes the container a " +
         "struct of that list, §7.1.4's embedder policy and a referrer policy, so a missing item is a clone " +
         "that sends referrers its creator's policy strips, or one that arrives unable " +
         "to resolve `'self'` against anything but this document's own address or claiming `unsafe-none` for a " +
         "creator that opted into cross-origin isolation");
  /* The parent of the navigable this instance is rooted in (HTML §7.3.1.3 Child navigables), a separate kind
     of fact from the container policy. `u` is the engine's encoding for no parent (a top-level traversable);
     anything else is the identity record the emitting engine wrote, relayed verbatim. SECURITY.md makes a
     cross-origin `<iframe>` the root of its own instance, so this is how such a root knows it is a child. No
     default: "the caller forgot" and "no parent" would become one value, reporting a frame as a page. */
  DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
         "an instance was started with no HTML §7.3.1.3 PARENT NAVIGABLE — every call site knows which of the " +
         "two answers applies (a create notice carries the identity its engine wrote; a reported root, a " +
         "rehydrated recipe and §7.3.2.3's swap have no embedder and state `u`), so an absent one is a caller " +
         "that skipped the question and a document that would present as a top-level page in the only " +
         "instance that holds it");
  /* HTML §7.2.2.4's opener, in the parent's grammar; `N` is the engine's encoding for none. */
  DCHECK(typeof openerNavigable === "string" && openerNavigable !== "",
         "an instance was started with no HTML §7.2.2.4 OPENER NAVIGABLE — a create notice carries the identity " +
         "its engine wrote and every other call site states `N`, so an absent one is a caller that skipped it");
  /* The navigable's container's answer. The container is an element in the creating instance's tree and cannot
     cross, so the creator runs Permissions Policy §9.5 Create a Permissions Policy for a navigable and this
     carries the result; `null` is that grammar's "no container". No default: §9.7 Define an inherited policy
     for feature in container at origin turns a null container into `Enabled` for every feature, which would
     hand a cross-origin frame its embedder's capabilities. */
  DCHECK(typeof containerPolicy === "string" && containerPolicy !== "",
         "an instance was started with no HTML §7.3.1.3 CONTAINER statement — Permissions Policy §9.5's " +
         "answer for its navigable and `null` are the two things a caller can say, and every call site knows " +
         "which applies (a create notice carries the answer the creating engine computed; a document nothing " +
         "embeds says `null`). An absent one is a caller that skipped the question and a frame that would be " +
         "granted every feature its embedder holds");
  /* The internal ancestor origin objects list (HTML §3.1.3 Ancestor origins) for the Document this instance
     builds, computed in the creating instance because it reads the parent document's list, its origin record
     and the container element, none of which survive serialization (an opaque origin is decided by identity).
     `none` means no ancestors. No default: an empty list claims this Document is at the top of its tree, so a
     cross-origin frame would answer `location.ancestorOrigins` with `[]`. */
  DCHECK(typeof ancestorOrigins === "string" && ancestorOrigins !== "",
         "an instance was started with no HTML §3.1.3 ANCESTOR ORIGINS statement — the composed list and " +
         "`none` are the two things a caller can say, and every call site knows which applies (a create " +
         "notice carries the list the creating engine composed; a document nothing embeds says `none`). An " +
         "absent one is a caller that skipped the question and a frame that would report itself as the top of " +
         "its own tree");
  /* The creation sandboxing flag set (HTML §7.1.5 Sandboxing) for that navigable, not an item of the policy
     container (§7.3.2.1 Creating browsing contexts sets them in different steps). It reads the embedder
     element's flags, so the creating engine computes it and this carries the answer; `none` is the empty set.
     No default: an empty set claims nothing is sandboxed, so a cross-origin `<iframe sandbox>` would run
     scripts and submit forms its embedder forbids. */
  DCHECK(typeof creationSandboxFlags === "string" && creationSandboxFlags !== "",
         "an instance was started with no HTML §7.1.5 CREATION SANDBOXING FLAG SET — the composed set and " +
         "`none` are the two things a caller can say, and every call site knows which applies (a create " +
         "notice carries the set the creating engine computed; a document nothing embeds says `none`). An " +
         "absent one is a caller that skipped the question and a sandboxed frame with its sandbox deleted");
  /* Whether another instance holds a WindowProxy for this document. A referenced document's last flow reports
     itself host-owed instead of finishing (solver/engine.h's `engine_set_referenced`, via `SetReferenced`), so
     a later `windowproxy.get`, delivery or `w.length` still has a timeline to be answered in; only the zone
     holding the routing table knows. It is stated per call site because the answer differs: a group switch
     (HTML §7.1.3.2 Browsing context group switches due to opener policy) mints a name nothing holds a proxy
     for. Understating it drains a peer about to be read (the crash lands in the asking instance);
     overstating holds a frontier open until the pool releases it, costing a slot and truncating nothing. */
  DCHECK(referenced === 0 || referenced === 1,
         "an instance was started without stating whether a peer holds a reference into its document — the " +
         "flag decides whether this instance's timelines may finish, so an unstated one is either a peer that " +
         "drains before it is asked anything or a document held open for a question nobody can ask, and " +
         "neither is visible from here");
  const cluster = clusterKeyOf(msg);
  /* The pool is the register of who holds what; every caller has already asked whether this cluster has an
     instance (admit's `hostClusterOf`, the create notice's, and the cold tier's per-recipe key). */
  DCHECK(hostClusterOf(cluster) === null,
         "a second instance was started for an agent cluster that already has one (" + cluster + ") — two " +
         "heaps for one similar-origin window agent is the split SECURITY.md's one-instance-per-cluster rule " +
         "exists to forbid, and the pool already held the answer on the line that asked");
  /* The document id is minted before the first await, because the reservation must be answerable by name
     (`hostHolderOf` routes cross-document messages) from the instant it exists. A live root document keeps the
     name the browser gave it, so a document delivered twice (content.js re-ships CONTENT_SEED on RESHIP) is
     recognised rather than given a second instance; a peer-created document arrives named (`docName`); a
     rehydrated recipe takes the counter. */
  const docId = docName || (msg && msg.documentId ? String(msg.documentId) : String(++nextDocumentId));
  const eng = engineReserve(cluster, docId, msg, referenced);
  /* The provisioning, the only part that suspends. The promise says when, never carries the instance (the
     caller already holds the record), and settles on both outcomes because hostSchedule's wait arm waits on
     it: an engine abort resolves it, recorded through engineBootFailed, which also removes the reservation;
     only an invariant abort rejects. It is assigned before anything can observe the record, since this function
     is synchronous up to the async body's first await. */
  eng._readyP = (async () => {
    try {
      bootSpanMark(eng.docId, "provision");
      eng.r = await self.rendererLaunch(cluster);
      bootSpanMark(eng.docId, "rendererReady");
      await engineRoot(eng, code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable,
                       openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags);
      /* A Clear that landed mid-boot is honoured here: hostClear cannot destroy a renderer with a call
         outstanding, so it marks the record and the owner of that call removes the frame. */
      if (eng._dropped) engineFrameDestroy(eng);
      _reserveStats.rooted++;
    } catch (e) {
      engineBootFailed(eng, e);
      RETHROW_FATAL(e);
      /* An engine abort while resuming a residue (cold_resume runs inside Begin) is recorded on that entry, as
         a crashed step's is at `finish`. */
      if (eng._residueEntry !== null) {
        const lines = e && Array.isArray(e.rendererLines) ? e.rendererLines : [];
        await frontierRecordCrash(eng._residueEntry, rootWhyLine(lines) || String((e && e.message) || e));
      }
    }
  })();
  return eng;
}
/* The pool slot, taken synchronously before any suspension, so two arrivals for one agent cluster cannot both
   pass the `hostClusterOf` check and both build: two heaps for one similar-origin window agent are
   unrepresentable (a node inserted across them, a closure neither can call). The second arrival finds the
   record and joins it. The record carries only what is known without asking the instance: `residentBytes`
   and `level1` are absent until engineRecordFacts states them, and readers check `state` first. */
/* Every document name this zone provisioned into an instance this session (a reservation's root, a joined
   document), with the browsing-context group it was provisioned in and, once its instance left the pool, how.
   It answers whether a name an engine states was ever a document here: a name it does not hold for the
   engine's group is the engine's invention, and one it holds as ended is this zone's own lifecycle. Cleared
   with the pool. */
/* `ended` is "finalizing" from the pool's splice until engineFinalize answers, then "finalized" for a run that
   ended normally or "crashed" for one that aborted or was refused; a failed boot is "crashed" too. */
const _provisioned = new Map();   // name -> { groupId, ended: null | "finalizing" | "finalized" | "crashed" }
function provisionedEnd(eng, how) {
  for (const n of [eng.docId, ...eng.joinedDocIds]) {
    const r = _provisioned.get(n);
    if (r !== undefined) r.ended = how;
  }
}
/* What a document name an engine states as a peer's is, when no instance in the pool holds it: null when this
   zone provisioned it in `groupId` and it has since ended (the caller reads how), or the reason the name is
   the engine's statement of a document that never existed in that group. A provisioned name no instance holds
   has always ended, since the pool is spliced only where `ended` is written. */
function provisionedRefusal(name, groupId) {
  const r = _provisioned.get(name);
  if (r === undefined) return "names the document `" + name + "`, which this zone never provisioned";
  if (r.groupId !== groupId) return "names the document `" + name + "`, which this zone provisioned in another browsing-context group";
  return null;
}
function engineReserve(cluster, docId, msg, referenced) {
  /* `_readyP` is null here and filled by engineCreate on the next line; a placeholder promise would settle at
     a different moment from the provisioning. The wait arm asserts it is present. */
  /* `joinedDocIds` is the host's half of main.c's `g_joined_ctx`/`g_joined_dom`, declared empty: an instance
     holds one document per realm and `docId` names only the root, so every joined document must answer for it
     in `hostHolderOf`, the routing table a `windowproxy.post` resolves through. */
  /* The wipe generation this instance observes under, taken once before the first await: what a merge asks is
     whether this instance was running before a Clear, which is the same for every advance it produces.
     `onFrontierAdvance` (the partial-snapshot merge and the finalize merge of an instance with no live caller)
     compares it, as `_dispatchDocument` does for the terminal path. */
  DCHECK(typeof self.frontierEpoch === "function",
         "the trusted zone states no frontier epoch — it is what tells an advance from this instance apart " +
         "from one observed before the user emptied the store, and without it every finding this engine " +
         "produces would merge into whatever the store holds when it lands");
  /* `topDocId` is which document is currently the active document of this group's top-level traversable;
     `docId` is the root the instance was reserved for and never changes (the crash record and cold recipe are
     written under it). A same-origin navigation changes the first (HTML §7.4.6.1 Updating the traversable
     replaces the Document and keeps the navigable). */
  /* `_resumed` is null until `begin` decides how many parked flows seeded the frontier; zero is a different
     fact (no residue handed over, so a boot flow was seeded). */
  /* `referenced` is on the reservation, not an engineRoot parameter, because it outlives rooting: the
     create-notice join arm reads it to refuse adding a peer-referenced document to an instance entitled to
     drain. engineRoot turns it into the ABI call. */
  /* The cold-tier lookup fields, null until engineRoot's frontier read decides them; a boot that dies first
     carries the nulls unchanged, which says when it died. */
  /* `records` is this zone's record of each document the instance holds (lib/document-record.js), which a
     request or notice is judged against by its asking or issuing document (askerRecord); written by
     engineRoot and engineJoin only. An instance has no record of its own. */
  const eng = { state: "booting", cluster, docId, topDocId: docId, joinedDocIds: [], msg,
                records: new Map(),
                /* HTML §7.2.2.4's opener each navigable of this instance may carry, by this zone's record
                   ({doc, origin} or null): written beside `records` from the opener this zone provisioned, and
                   by `navigable.open` for a navigable the engine holds (engineOpenerLookup). */
                openers: new Map(),
                groupId: msg.groupId, _resumed: null, referenced,
                /* Whether engineRoot has opened this instance's jar to zoneRouteSetCookies. */
                cookiesOpen: false,
                _coldLookup: null, _coldOther: null, _bundleId: null, _residueEntry: null,
                origin: msg.origin, _resolvers: [], _remoteAsked: new Set(),
                /* The requests this zone has asked for and not yet answered to the engine, keyed as the
                   answering seam is keyed. The engine re-lists a request with neither value nor refusal every
                   round (solver/engine.c's `skip` is `!u || PEND_HAVE_VALUE || declined`), so a door that issues
                   without waiting must dedup, or one park becomes one request per round. engine/host/wpt_runner.c
                   drives the same ABI with the same table and keys (see engineIssue). */
                _inflight: new Map(),
                _epoch: self.frontierEpoch(), r: null, _readyP: null,
                /* How much of its partition of the product's known identities this instance has been handed
                   (lib/novelty-set.js's noveltyOwed advances it). */
                _novelty: { spelling: null, n: 0 },
                /* …and the drive counts it last reported, so a repeated result adds each drive once. */
                _noveltyCounted: new Map(),
                /* Egress asked and refused, per rule, declared here so a run that refused nothing still carries
                   the map. `asked` is raised at the call and `declined` at the refusal in engineServiceFetch's
                   one loop, so the sum is bounded by `asked`. Both are lifetime counts of ask events, not of
                   distinct addresses (an @S candidate re-fire asks again). */
                _egress: { asked: 0, declined: Object.create(null) } };
  _pool.push(eng);
  _provisioned.set(docId, { groupId: msg.groupId, ended: null });
  _reserveStats.made++;
  const n = _bootingCount();
  if (n > _reserveStats.peakBooting) _reserveStats.peakBooting = n;
  return eng;
}
/* A reservation that did not become an instance leaves the pool here, for every call site: one left behind
   would be joined by every later arrival for that cluster, which would wait for ever on a failed boot. */
function engineBootFailed(eng, e) {
  const i = _pool.indexOf(eng);
  DCHECK(i >= 0 || eng._dropped,
         "a reservation failed to boot and was already out of the pool with no Clear having taken it — the " +
         "slot it holds is this function's to release, so something else removing it means two owners for one " +
         "record and the surviving one will free a frame twice");
  if (i >= 0) _pool.splice(i, 1);
  provisionedEnd(eng, "crashed");
  zoneReclaimHeldCookies(eng.groupId);
  eng.state = "failed";
  childRequestsClose(eng);
  if (_documentReferrerPolicies.has(eng.docId)) documentPolicySettle(eng.docId, null);
  /* The frame goes with it. renderer-host.js reclaims its own frame (and frees the cluster in the registry)
     when the boot handshake fails, so `r` is null exactly when there is nothing to remove. */
  if (eng.r) engineFrameDestroy(eng);
  /* One instance failing is one crash however many documents were waiting on it: the banner fires once and a
     record is built per caller. */
  const m = String((e && e.message) || e);
  /* The cause: a boot that aborts inside the engine rejects an ABI call, and renderer-host.js attaches the
     frame's last lines (`e.rendererLines`), where the C CHECK/DCHECK printed its @WHY root line. A failure that
     never reached an ABI call (rendererLaunch, or an assert here) has no lines and its message is the cause. */
  const _rl = e ? e.rendererLines : undefined;
  DCHECK(_rl === undefined || Array.isArray(_rl),
         "a boot failure carried a `rendererLines` that is not a list of the frame's output — it is the only " +
         "place the engine's own @WHY survives a rejected ABI call, and a shape this cannot scan is that root " +
         "line being discarded at the one site that holds it");
  const root = _rl === undefined ? "" : rootWhyLine(_rl);
  /* A boot engineRoot failed because this zone refused the instance's first reply: engineRefuse has bannered
     it, and each record carries the refusal under its own stage. */
  const refused = !!(e && e.zoneRefused);
  DCHECK(refused === !!eng._crashed,
         "a boot failed with refusal " + refused + " on an instance whose crashed mark is " + !!eng._crashed +
         " — only engineRefuse marks an instance before it becomes hot, and engineRoot throws a refusal for it");
  const err = refused ? m : root ? (m + " | ROOT: " + root) : m;
  if (!refused) crashBanner("create", err);
  for (const w of eng._resolvers) w.resolve(crashRecord(refused ? "result-refused" : "create", err, w.msg, eng));
  eng._resolvers.length = 0;
  _reserveStats.failed++;
}
/* How many parked flows seeded this session's frontier, read once where the seeding happened. solver/cold.c
   prints `@RESUMED <n>` from cold_resume, called only for a non-empty residue: `n >= 1` after a rebuild (it
   DCHECKs `flows > 0`), and 0 for a residue refused whole for an older build's name spelling (solver/cold.h).
   An absent line and a zero are distinct. The line is the engine's statement: answers the count, or the
   reason the line is refused (a malformed or repeated line, or one that disagrees with `asked`, whether this
   zone composed the `recipes` argument), which engineRoot refuses the instance with. */
function engineResumeCount(lines, asked) {
  let n = -1;
  for (const raw of lines) {
    const ln = String(raw);
    if (!ln.startsWith("@RESUMED ")) continue;
    const tail = ln.slice(9);
    /* solver/cold.c writes the count with one `%ld`, once per session. */
    if (!/^[0-9]+$/.test(tail)) return "the engine printed an @RESUMED count that is not a decimal number (`" + tail + "`)";
    if (n >= 0) return "the engine printed @RESUMED twice (" + n + " then " + tail + ") in one session";
    n = Number(tail);
  }
  /* solver/engine.c seeds from a residue or from a boot flow and never both: a residue with no line is every
     parked flow dropped between the store and the frontier, and a line with none is flows standing on decision
     vectors this document was never handed. */
  if (asked && n < 0) return "this zone handed the engine a parked residue and the engine reported no rebuild";
  if (!asked && n >= 0) return "the engine reported resuming " + n + " parked flows into a session seeded with none";
  return n >= 0 ? n : 0;
}
/* The error engineRoot throws for an instance it refused before the instance became hot (engineRefuse has
   marked it), which engineBootFailed reads as a refusal rather than an abort. */
function engineBootRefusal(why) {
  return Object.assign(new Error(why), { zoneRefused: true });
}
/* An instance's GetReferrerPolicy answer for `docName`, settled into the document's entry. It is the
   instance's statement (qjs_referrer_policy), so a word that is not a Referrer Policy §3 policy settles null
   and refuses the instance. Answers the refusal, or null. */
function enginePolicySettle(eng, docName, policy) {
  if (policy === "" || REFERRER_POLICIES.indexOf(policy) >= 0) { documentPolicySettle(docName, policy); return null; }
  const why = "qjs_referrer_policy answered " + JSON.stringify(policy) + " for " + docName +
              ", which is not a Referrer Policy §3 policy";
  documentPolicySettle(docName, null);
  if (!eng._crashed) engineRefuse(eng, why);
  return why;
}
/* The local is `rend` and the field `eng.r` because the fetch closures below bind `const r` to Fetch's reply
   record; a renderer named `r` would be shadowed there. engineRoot fills the reservation and returns nothing:
   the pool's record is the one every question about this cluster has been answered by since provisioning
   began, and a second object would give one document two records. */
async function engineRoot(eng, code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable,
                          openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags) {
  const rend = eng.r;
  DCHECK(rend && rend.name === eng.cluster,
         "a renderer was provisioned under a name that is not its instance's agent cluster — the frame's " +
         "title is how this zone identifies which instance the pool is talking about, and the pool keys the " +
         "same instance by `cluster`, so two spellings of one identity is a routing table that disagrees with " +
         "the DOM");
  /* The renderer's line buffer itself: renderer-host.js appends every drained line as each reply lands, so
     engineCrash's root-@WHY scan, streamPartial's @RESULT scan and linesToAnalysis read what the frame printed,
     in order. */
  const lines = rend.lines;
  // Phase 1: parse and boot; the engine computes the bundle identity from its Lexbor <script> scan.
  /* The whole response header list crosses as HTTP field lines, not single headers: several algorithms read
     different names from the same list (HTML §7.1.7's embedder policy, §7.5.1 Shared document creation
     infrastructure's opener policy and `Origin-Agent-Cluster` for §8.1.2.2's agent cluster key), and Fetch's
     `get` decides what a repeated header means. The browser's `Headers` iteration has already combined repeats
     as `get` does, so one line per name gives the engine's item parse the browser's verdict. The engine parses
     it back into a header list (core/fetch/headers.c) and reads it once, into HTML §7.4.2.1 Supporting
     concepts' navigation params, which §7.4.5 Populating a session history entry creates from the response. */
  const _headers = responseFieldLines(msg && msg.responseHeaders);
  // The document id is minted on the reservation (engineCreate), so `hostHolderOf` routes to a booting
  // document. It is unique across the instances alive in this offscreen, the set that can message each other;
  // it is not persisted because a parked foreign segment does not yet outlive a session.
  // The top-level creation URL (HTML §8.1.3.1 Environments), always browser-stated: a peer-created document
  // carries its creator's decision on the navigable.create notice; a content-script report carries the tab's
  // url from sender.tab.url; a rehydrated recipe carries what its session recorded. HTML §8.1.3.5 Secure
  // contexts reads it to decide which Web IDL §3.3.13 [SecureContext] members exist, so the engine refuses "".
  const _tlu = topLevelUrl || (msg && msg.topLevelUrl) || "";
  // No `code` reaches qjs_init: identity and the script inventory are the engine's Lexbor <script> scan of
  // `html`, since a concatenation of scripts cannot represent per-script scope.
  // The address, not the origin, crosses: it is the API base URL (HTML §8.1.3.2 Environment settings objects)
  // every relative URL resolves against, and the engine derives the origin from it (its own url.c), so the
  // principal and the address are one fact.
  /* qjs_init's return value is read: its C body is CHECKs that abort, so it returns 0, and anything else is an
     entry reporting a failure this zone must hear. */
  /* The document crosses as bytes, since `qjs_init` takes a byte sequence and its length, and every live
     document arrives as the response's own bytes from navigationLoad. The string arm serves a cold-tier entry
     an earlier session parked as characters; it encodes, never decodes, because decoding would be this zone
     running HTML §13.2.3.2 Determining the character encoding badly. Both shapes are asserted, never defaulted
     to "". The buffer is not transferred: `eng.html` keeps it for finish() to write to IndexedDB as the cold
     recipe's document (mojom.js states this as a property of the declared type). */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a document reached qjs_init as neither a byte sequence nor characters — navigationLoad ships a " +
         "response body and a cold-tier entry may hold characters, so anything else is a producer that " +
         "stopped producing a document, with nothing downstream to notice but an empty finding set");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* A U+0000 in these bytes is input, not an invariant: HTML §13.2.3.5 Preprocessing the input stream defines
     how each tokenizer state handles it. `qjs_init`/`qjs_join` take `(bytes, len)` and the C entry DCHECKs the
     guard byte at `bytes[len]`, so the length and the C read cannot disagree.
     Named residual: the engine decodes these bytes as UTF-8 rather than running HTML §13.2.3.2 Determining
     the character encoding. Next diff builds the sniff over this byte sequence. Absence shows as non-UTF-8
     pages whose text and attribute values arrive mojibaked. */
  /* The address is asserted, never defaulted: `originOf("")` is "", a frontier key every unidentifiable
     document would share. The engine's entry CHECKs that it parses; this fails in the zone that made the hole. */
  DCHECK(!!msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "a document reached qjs_init with no address — §4.4's document address is what the engine derives " +
         "this document's ORIGIN from (§4.7's serialization, its own url.c) and what every relative URL the " +
         "bundle builds resolves against, so a document without one is analysed behind no principal at all");
  /* The inherited container as wire fields beside `_headers`, because it is not part of the response: it is
     HTML §7.1.7's clone for a Document with a creator, and the engine decides which list the Document gets. The
     empty pair states that there is no creator. */
  /* The embedder policy item rides with the list. A document with no creator (a reported root, a rehydrated
     recipe, a group swap) gets "a new embedder policy" (HTML §7.1.7), which NEW_EMBEDDER_POLICY spells. */
  const _initEp = inherited ? inherited.embedder : NEW_EMBEDDER_POLICY;
  /* The parent navigable and the container answer are relayed beside the container: they are what only this
     zone knows about a document it did not root (whether it is nested in another instance's navigable), and
     engineCreate asserts them. */
  /* What the product already holds for this cluster, handed before Init so the root's own parse is judged
     against it too: qjs_novelty_learn is valid before qjs_init. */
  await noveltyReady();
  await noveltyHand(eng);
  const _init = await rend.renderer.init({
    document: _doc, url: msg.sourceUrl, docId: eng.docId, headers: _headers, topLevelUrl: _tlu,
    inheritedCsp: inherited ? inherited.csp : "",
    inheritedCspReportOnly: inherited ? inherited.cspReportOnly : "",
    inheritedCspSelfOrigin: inherited ? inherited.selfOrigin : "",
    inheritedCoep: _initEp.value, inheritedCoepEndpoint: _initEp.endpoint,
    inheritedCoepReportOnly: _initEp.reportOnlyValue,
    inheritedCoepReportOnlyEndpoint: _initEp.reportOnlyEndpoint,
    inheritedReferrerPolicy: inherited ? inherited.referrerPolicy : DEFAULT_REFERRER_POLICY,
    parentNavigable, openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags });
  /* The engine's entry CHECKs every precondition and aborts, so a non-zero return is a contract this zone does
     not speak, refused as the instance's answer. */
  if (_init.rc !== 0) {
    const why = "qjs_init answered rc " + JSON.stringify(_init.rc) + ", which this zone has no handling for";
    engineRefuse(eng, why);
    throw engineBootRefusal(why);
  }
  bootSpanMark(eng.docId, "init");
  /* The root document's load, which `Init` began: one work quantum per `Load`, each its own message, so this
     zone's event loop runs between two of them, and nothing else is sent to the instance until it answers 0. */
  for (;;) {
    const _more = (await rend.renderer.load()).more;
    if (_more === 0) break;
    if (_more !== 1) {
      const why = "qjs_load answered " + JSON.stringify(_more) + ", which is neither 1 (more) nor 0 (installed)";
      engineRefuse(eng, why);
      throw engineBootRefusal(why);
    }
  }
  bootSpanMark(eng.docId, "rootInstalled");
  /* Whether a peer holds a WindowProxy for this document, stated after `Init` and before `Begin`, the C entry's
     own bounds: there must be a document (`g_ctx`) and the frontier must not be seeded yet (`!g_begun`), since
     the flag decides whether the last timeline may finish. It is stated unconditionally, including 0, so the
     C flag is never left at its static initialiser. `qjs_set_referenced` survives a `Teardown`, so it is not
     restated after one. */
  await rend.renderer.setReferenced({ referenced: eng.referenced });
  /* And how this zone reached the document, stated in the same window for the same reason: every timeline's
     grade starts from it (solver/engine.h's engine_set_arrival), so a document reached past a forced gate learns
     its endpoints as `forced`. `msg.provenance` is that reach grade, asserted where the root is built. */
  DCHECK(typeof msg.arrival === "string" && msg.arrival.startsWith(msg.provenance + ","),
         "a document is being rooted with the arrival `" + String(msg.arrival) + "` beside the reach grade `" +
         String(msg.provenance) + "` — every composer of an AST_ANALYZE states both, the arrival's grade being " +
         "the reach (docRecordArrival)");
  /* This zone's record of the root document (lib/document-record.js), written once for the instance's life:
     the arrival is its wire form, so the marks the engine starts every timeline with and the floor the
     chokepoint holds the document's requests to are one statement. Kept on the instance, since a Clear can drop
     it and the browser's same document id can come back to a fresh one. */
  CHECK(eng.records.size === 0,
        "an instance's root document record was written twice — it is written once, when the root is " +
        "provisioned, and every request the document asks is judged against it");
  const rootDoc = docRecordOfArrival(msg.arrival, msg.sandbox, msg.addressSpace);
  CHECK(creationSandboxFlags === docRecordSandbox(rootDoc),
        "a document is being rooted with the flag set `" + String(creationSandboxFlags) + "` beside a record " +
        "whose set is `" + docRecordSandbox(rootDoc) + "` — the engine is handed the record's set, one statement");
  eng.records.set(eng.docId, rootDoc);
  eng.openers.set(eng.docId, { opener: docRecordNavigableIdentity(openerNavigable), refused: false });
  await rend.renderer.setArrival({ arrival: msg.arrival });
  /* And the Set-Cookie fields the zone holds for this cluster, the root document's own response's among them,
     in the same window: the jar has them before the root's first script runs. */
  await zoneOpenJar(eng);
  /* Never 0: document_bundle_id folds an empty scan to 1, since a 0 would key every unidentifiable document to
     one frontier entry. */
  /* `>>> 0` because the wire carries an i32 (mojom `int32 bundleId`) for a C `unsigned`; the reader
     reinterprets it. */
  const _bidRaw = (await rend.renderer.getBundleId()).bundleId >>> 0;
  /* A 0 would collide every document's frontier key with every other's. */
  if (_bidRaw === 0) {
    const why = "the engine answered a bundle id of 0, which document_bundle_id never returns";
    engineRefuse(eng, why);
    throw engineBootRefusal(why);
  }
  const _bid = _bidRaw.toString(36);
  /* The address parses in this zone too: qjs_init has CHECKed that the engine's url.c parsed it, so an empty
     origin here means the two parsers disagree about which document the key names. */
  DCHECK(originOf(msg && msg.sourceUrl) !== "",
         "this zone could not serialize an origin from a document address the engine's own url.c accepted — " +
         "the frontier key would name a document by a string the two parsers do not agree is one");
  await siteAsk(siteHostsOf([msg.sourceUrl, msg.origin]));
  const fkey = frontierKeyOf(msg.sourceUrl, msg.origin, msg.credentialed, _bid);
  /* One read that answers why a key missed beside what it found (see COLD_LOOKUP): frontierLookup asks the key
     and, in the same transaction, the keys stored for this address. The three fields are written here, at the
     one moment the answer exists. */
  const _look = persist ? await frontierLookup(fkey, msg.sourceUrl) : null;
  if (_look && _look.entry) await programDigest();
  const _cl = coldLookupOf(fkey, _look);
  eng._coldLookup = _cl.state; eng._coldOther = _cl.others; eng._bundleId = _bid;
  /* The word and the residue come from one read and are not asserted against each other: `hit` is defined by
     `_look.entry` and `prior` is `_look.entry`, so that comparison cannot fail. The assert that can fail is in
     `coldLookupOf`. */
  const prior = _look ? _look.entry : null;
  /* The residue belongs to this document: the engine seeds from these recipes instead of a boot flow, so a
     residue from another document would replay a path this program never took. Under the current key this is
     an identity; it fires if a term ever leaves the key. */
  DCHECK(!prior || prior.sourceUrl === msg.sourceUrl,
         "the cold tier answered this document's frontier key with a residue parked by a document at a " +
         "DIFFERENT address (" + (prior && prior.sourceUrl) + " vs " + msg.sourceUrl + ") — the engine seeds " +
         "from recipes instead of a boot flow, so this document would be replayed along another one's " +
         "decision vectors and never explored from its own first script");
  /* …and by a document of the same identity: a residue parked under another partition or credential state
     would replay that document's decisions under this one's session. */
  DCHECK(!prior || documentIdentityOf(prior.sourceUrl, prior.origin, prior.credentialed) ===
                   documentIdentityOf(msg.sourceUrl, msg.origin, msg.credentialed),
         "the cold tier answered this document's frontier key with a residue of another identity (" +
         (prior && documentIdentityOf(prior.sourceUrl, prior.origin, prior.credentialed)) + " vs " +
         documentIdentityOf(msg.sourceUrl, msg.origin, msg.credentialed) + ") — the key names the identity, " +
         "so the store and its key have parted");
  // Phase 2: seed the frontier, fresh or from parked recipes. Each step is one slice, ended at the work quantum.
  /* The residue handed over, named before it is sent: solver/engine.c seeds from recipes or a boot flow and
     never both, so "" is this zone's own statement that nothing was resumed. */
  const _residue = (prior && prior.recipes && _cl.state !== "crashed") ? prior.recipes : "";
  DCHECK(_residue === "" || !frontierCrashedHere(prior),
         "a residue this engine already aborted on is about to be resumed again — the lookup's `crashed` word " +
         "boots the session fresh, so this is the word and the residue disagreeing");
  /* Named before Begin, which is where cold_resume runs: an abort there is this residue's crash as much as one
     in a later step. */
  eng._residueEntry = _residue !== "" ? prior : null;
  await rend.renderer.begin({ recipes: _residue });
  bootSpanMark(eng.docId, "begun");
  /* An open referrer policy entry's answer, read after Begin and before any step so it is the parse's
     (documentReferrerPolicy). A live seed's root keeps the browser's documentId as its name (engineCreate). */
  DCHECK(!msg || typeof msg.seedUrl !== "string" || String(msg.documentId) === eng.docId,
         "a live seed was rooted under a name that is not its browser documentId — engineCreate keeps that name " +
         "for a reported root, and the seed's referrer policy entry is keyed by it");
  if (_documentReferrerPolicies.has(eng.docId)) {
    const why = enginePolicySettle(eng, eng.docId,
                                   (await rend.renderer.getReferrerPolicy({ docId: eng.docId })).referrerPolicy);
    if (why !== null) throw engineBootRefusal(why);
  }
  /* The engine's half: `cold_resume` prints `@RESUMED <n>`, and every reply drains the output with it (mojo.js
     `_envelope`), so the line is in `lines` when this await resolves and never again. It is read once here and
     linesToAnalysis reads the field. */
  const _resumedCount = engineResumeCount(lines, _residue !== "");
  if (typeof _resumedCount === "string") { engineRefuse(eng, _resumedCount); throw engineBootRefusal(_resumedCount); }
  eng._resumed = _resumedCount;
  // Dev-only verification hook (a real page never carries this query param): `?__forcepark=1` forces the
  // RAM-pressure park, so the cross-session round trip (park recipes, IndexedDB, restart, resume) is verifiable
  // without a 512MB working set. The param is part of the address and so of the frontier key: that address
  // names its own entry, and a rehydration re-derives the same key.
  // The park is deferred two dispatches so it fires mid-exploration, like a production park: a park before any
  // step writes one record, `f-,0`, which resumes as a flow indistinguishable from a fresh boot flow, so the
  // round trip would carry nothing. Two is the smallest count at which the boot flow has forked and the park
  // holds `s…` segment records; the property to preserve is "the park contains at least one `s` record".
  // Only on the initial park: firing while resuming would re-park the rehydrated recipes for ever.
  let _forceparkSteps = 0;
  if (msg && typeof msg.sourceUrl === "string" && /[?&]__forcepark=1\b/.test(msg.sourceUrl) && !(prior && prior.recipes)) {
    _forceparkSteps = 2;   // after boot and the first flow burst: flows have run and suspended (real decision vectors), not drained
  }
  /* The chokepoint and the document's address are a load order, asserted once here as a crash. Answering
     either with a network error would tell the page's code its server was unreachable and explore a failure
     path under a wiring defect. */
  DCHECK(typeof self.safeFetch === "function",
         "an engine is being driven in a realm where `safeFetch` is not a function — SECURITY.md puts every " +
         "byte of network through that chokepoint, so this zone cannot answer a single park, and the two " +
         "seams that used to report it as a network error told every page's code that its server was " +
         "unreachable. It is a load order: `extension/lib/safe-fetch.js` has not been loaded into this realm");
  DCHECK(msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "an engine is being driven for a document with no source URL — every park's address is resolved " +
         "against it (`new URL(u, msg.sourceUrl)`), so without one nothing this frontier asks for can be " +
         "named, and answering that with a network error reports an unreachable server for a request this " +
         "zone could not even address");
  /* The reply record the engine parses, the shape every host of this engine delivers:
     `{status, statusText, headers: [[name, value], …], urlList, computedType}` plus the body's bytes beside it,
     as the C hosts build with fetch_reply_new. The URL list (Fetch §2.2.6 Responses) is what `response.url`
     and `response.redirected` read. `null` is a network error (the engine rejects with Fetch §5.6 Fetch
     methods' TypeError), never an empty 200.
     The body is not in the record: Fetch §2.2.4 Bodies makes a body's source a byte sequence, which JSON cannot
     carry without a codec or a decode, and a decode here would run in the zone that owns SOP/CORS and no
     semantics, before the engine's own decode sees the declared charset. So the record travels as text and the
     bytes as bytes, copied straight into the engine's linear memory. */
  /* The request is the (method, url) pair the flow parked on; `method` arrives normalized (Fetch §2.2.1
     Methods). A method this zone cannot issue is refused before the call and never downgraded: safeFetch is
     GET only (SECURITY.md), so answering a POST's key with a GET's reply would be a response the server never
     gave. On this seam the refusal is spelled `null`, because core/fetch builds any object into a Response,
     so a status-0 record would resolve `fetch()` with a status no server returns. fetchedXhr spells a network
     refusal as an answer with no body, which xml_http_request.c's xhr_take_reply leaves as the initial network
     error of XHR §3 "Interface XMLHttpRequest", and a decline as `{declined}`, which goes to `hostDecline`. */
  /* The request's destination travels with it (Fetch §2.2.5 Requests): safe-fetch.js decides the CORB class
     from §2.2.5's script-like predicate over it, so it is passed through, not reduced to a boolean. */
  const fetched = async (method, u, destination, provenance, pinned, driven, credentials, mode, referrerPolicy,
                         referrer, asked, signal) => {
    DCHECK(signal instanceof AbortSignal,
           "a pending request reached the fetch relay with no abort signal — engineIssue owns one per send, and " +
           "a `fetch.cancel` notice ends the send through it");
    DCHECK(pinned === "pinned" || pinned === "unpinned",
           "a pending request reached the fetch relay carrying no witness mark — solver/engine.h states " +
           "`pinned` or `unpinned` on every line of the pending join, and the chokepoint reads it WITH the " +
           "destination to decide whether an act may be spent on an address this engine may have composed. " +
           "A relay that dropped it would have safe-fetch.js answer from `unstated`, which is the word for " +
           "an act that never had one");
    DCHECK(typeof destination === "string",
           "a pending request reached the chokepoint with no DESTINATION — every GetPending line carries one " +
           "(pendingRequest) and Fetch §2.2.5 makes the destination part of the request, so a caller that " +
           "omits it is one whose code load would be fetched as data and compiled. The empty string is a real " +
           "destination and means DATA");
    DCHECK(typeof method === "string" && method !== "",
           "a pending request reached the chokepoint with no method — every GetPending line carries one " +
           "(pendingRequest) and the (method, url) pair is what the flow parked on, so a request whose method " +
           "is unknown can be neither refused nor issued");
    /* The credentials mode is passed through for the same reason; reducing it with this zone's willingness is
       the chokepoint's job (`_credentialedOf`). */
    DCHECK(typeof credentials === "string" && credentials !== "",
           "a pending request reached the chokepoint with no CREDENTIALS MODE — every GetPending line carries " +
           "one (pendingRequest), Fetch §2.2.5 \"Requests\" gives every request one, and only the algorithm " +
           "that created it knows which. A caller that drops it hands `safe-fetch.js` a request it can decide " +
           "nothing about");
    /* The method half of the firing question is asked of the chokepoint (`safeFetchMethodRefusal`), so this
       host and engine/trusted.mjs answer it once, in the chokepoint's refusal vocabulary, and the flow stays
       parked rather than being told the server was unreachable. */
    /* The asking documents' judgement (askerRecord). A refusal declines under its closed token, which the
       egress histogram counts, and its sentence names the document that is not one of this instance's. */
    if (asked.refusal) {
      console.warn("[bridge] " + method + " " + u + " is refused: " + asked.refusal.text);
      return { refusal: { kind: "decline", reason: asked.refusal.token } };
    }
    const doc = asked.doc;
    DCHECK(docRecordIs(doc), "a pending request reached the fetch relay with no asking document's record");
    const _mRefusal = self.safeFetchMethodRefusal(method);
    if (_mRefusal) return { refusal: _mRefusal };
    /* A holed address is a park carrying an address the engine never determined, so there is no request to
       make and nothing for the chokepoint to grade; it is answered as a network error.
       Named residual: that answer sends the page's code down a failure path for a request nobody could make.
       Next diff stops the engine listing such a park to a host at all, treating the address as a shape (the
       @H shape rules). Absence shows as holed addresses that never reach the wire yet whose callers' catch
       arms are explored. */
    if (hasHole(u)) return null;
    try {
      const abs = new URL(u, msg.sourceUrl).href;
      // Every park takes one shape; the request's own destination decides the chokepoint's rule (CORB for a
      // code load), and credentials are stated below.
      /* The page's requests are uncredentialed (`credentialed: false`, stated, not read): `msg.credentialed`
         says whether the DOCUMENT load carried the session and is not this question. The request origin is
         `eng.origin`, the agent-cluster origin this zone assigned the instance from its own record (the
         browser's MessageSender.origin for a live document, the origin of the URL this zone loaded for a child),
         never `originOf(msg.sourceUrl)`, which would give a sandboxed iframe its embedder's origin, and never a
         word the engine states. safe-fetch.js reads it for Fetch §4.1 Main fetch's response tainting (§4.10
         CORS check for a `cors` request, an opaque response for a cross-origin `no-cors` one) and for CORB's
         same-origin exemption. */
      CHECK(typeof eng.origin === "string",
            "a page request is about to be judged with no origin on the instance that made it — the zone " +
            "assigns every instance its cluster origin at creation, and the chokepoint decides from it what " +
            "the page may read of another origin's reply");
      /* One shape for every park: the destination decides which rule the chokepoint applies and the policy on
         whether a code load may carry the session is the chokepoint's (HTML §8.1.4.2 Fetching scripts gives a
         classic script `same-origin` credentials). */
      /* `provenance` is what the request is evidence of, composed by the engine at the park from HTML §4.12.1.1
         Processing model's parser-inserted (a script whose parser document is non-null) and the parking flow's
         `path_forced` (solver/engine.h). Relayed, never re-derived or defaulted: the chokepoint holds the firing
         decision and an unknown value is fatal there. */
      /* `credentialed: false` is stated, not read (see the finding above). */
      /* `credentials` is the request's own mode (Fetch §2.2.5 Requests), relayed off the pending line; its
         conjunction with `credentialed` is safe-fetch.js's and can only narrow, so it makes `omit` parks
         refusable and `include` ones honest once the principal above is wired. */
      /* `doc` is this zone's record of the documents asking this park (askerRecord over the line's ASKERS),
         distinct from `provenance` (the engine's word about the flow's path inside the document): a page this
         tool opened makes requests the engine grades `observed`. */
      /* `actor: "page"`: this frame answers a park the engine put on its pending line, and the engine parks on
         what the analysed document's code does. Stated so safe-fetch.js can assert it; with `witness` unpinned it
         is the row the owner's value arm turns on. */
      /* `mode` is the request's own (Fetch §2.2.5 Requests), off the pending line, relayed: the engine states
         what the request IS, and the chokepoint's `_modeOf` CHECKs the word. */
      /* `useCorsPreflight: false`: Fetch §5.4 "Request class" sets that flag only for a ReadableStream body,
         and a request on this seam is a GET, which §5.4 refuses a body. */
      /* `driven` is the engine's drive mark off the same line (solver/engine.h's PENDING_DRIVEN_*), relayed:
         the chokepoint's `actor` row reads it. */
      const opts = { pageOrigin: eng.origin, destination, mode, provenance, pinned,
                     driven, credentials, actor: "page", useCorsPreflight: false,
                     doc, credentialed: false, signal,
                     identity: await pageRequestIdentity(stampOrigin(eng.origin), msg.sourceUrl, "engine:" + eng.docId, abs, credentials, mode,
                                                   referrerPolicy, referrer) };
      const r = await self.safeFetch(abs, opts);
      /* The chokepoint's record is fixed: safe-fetch.js returns {ok,status,statusText,headers,body,urlList} on
         every path, blocked ones included, so a malformed answer is asserted rather than turned into a network
         error. The body is bytes; a string would mean safeFetch ran `text()` (Fetch §5.3 Body mixin) and the
         original bytes are gone. */
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number",
             "safeFetch answered with something other than its reply record — the engine builds a Response " +
             "out of this and a page reads status/headers/body off it, and §2.2.4's body source is a BYTE " +
             "SEQUENCE");
      /* The URL list straight from the chokepoint: it always holds at least the requested URL (Fetch §4.1 Main
         fetch: an empty internal URL list is set to a clone of the request's), and the engine DCHECKs that too. */
      DCHECK(Array.isArray(r.urlList) && r.urlList.length >= 1,
             "safeFetch answered a reply with no URL list — response.url and response.redirected are read off " +
             "nothing else, and the engine would report every redirect as none");
      DCHECK(r.headers && typeof r.headers === "object",
             "safeFetch answered a reply with no header map — the engine's Headers record is built from it");
      /* The type the chokepoint computed, the one fact on this record the engine may not derive itself:
         safeFetch ran the sniff and the renderer is told the answer. The empty string is MIME Sniffing §5.1
         Interpreting the resource metadata's "the supplied MIME type is undefined", a positive answer. */
      DCHECK(typeof r.computedType === "string",
             "safeFetch answered a reply with no computed content type — solver/reply_decode.c reads this " +
             "field instead of re-deriving a type from the raw header, so an absent stamp is a producer that " +
             "failed and never a resource whose type is unknown");
      /* The status message's empty value is a real answer (HTTP/2 and HTTP/3 carry no reason phrase), and the
         blocked arms put their refusal reason there, so it is asserted, never defaulted. */
      DCHECK(typeof r.statusText === "string",
             "safeFetch answered a reply with no statusText — it is written on every path (the blocked arms " +
             "carry their REFUSAL REASON in it), and an empty string is a legitimate answer from any HTTP/2 " +
             "response, so an absent one cannot be defaulted without erasing the difference");
      /* Status 0 is not a reply: the chokepoint answers it when the request never went on the wire (bad URL,
         blocked scheme, private target, CORB), and it crosses as `null`, the network error. */
      /* Which kind of non-reply it is, stated by the chokepoint (`refusal.kind`) and never re-derived from
         `statusText`. A refusal a browser also makes (a `file:` scheme, a CORS failure, CORB) is a network
         error, which is fidelity. A refusal only this tool makes (the firing policy, the destructive deny list)
         has no browser behind it, so it is a `decline`: the flow stays parked rather than being told the origin
         failed. */
      DCHECK((r.status === 0) === (r.refusal !== null),
             "safeFetch answered with a status and a refusal that disagree — `status: 0` is the one status no " +
             "HTTP response has and is what every refusal answers, and `refusal` is the same fact graded, so " +
             "a record carrying one without the other is a decline arriving as a reply or a reply arriving as " +
             "a request that never happened");
      if (r.refusal) {
        DCHECK(r.refusal.kind === "network" || r.refusal.kind === "decline",
               "safeFetch graded a refusal `" + r.refusal.kind + "`, which is neither word it states — a " +
               "third grade would take whichever arm this branch happens to be written with as its else, and " +
               "that arm fabricates a network error for a request nobody sent");
        DCHECK(typeof r.refusal.reason === "string" && r.refusal.reason === r.statusText,
               "safeFetch graded a refusal whose reason is not the one on the record — the reason is the only " +
               "account a person ever gets of a request this zone did not make, and two spellings of it are " +
               "two answers to which rule refused");
        if (r.refusal.kind === "decline") return { refusal: r.refusal };
      }
      if (r.status === 0) return null;
      /* Two pieces of one reply crossing on two channels: `meta` is the JSON qjs_provide parses and `bytes` is
         copied into the engine's heap beside it. Returned together so neither is delivered without the other. */
      /* `type` is the filtered response the chokepoint chose (Fetch §2.2.6 Responses): `basic`, `cors` (its
         header list already narrowed) or `opaque`; the engine builds a `fetch()`'s Response from it. */
      DCHECK(r.type === "basic" || r.type === "cors" || r.type === "opaque",
             "safeFetch answered a reply with no filtered-response type — it states one on every reply, and " +
             "the engine reads it to decide what the page may read");
      return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers),
                       urlList: r.urlList, computedType: r.computedType, type: r.type },
               bytes: r.body };
    } catch (e) {
      /* A thrown fetch is a network error and crosses as `null`; an invariant abort thrown through this catch
         is not, so it is rethrown. */
      RETHROW_FATAL(e);
      return null;
    }
  };
  // The response (HTML §7.4.5 Populating a session history entry) for a child navigable this engine created,
  // through the same navigationLoad as the seed that rooted this instance. `fetched` cannot serve it: a child
  // document needs its header list, since a document with no policy would report CSP-blocked sinks as
  // exploitable. Both principals are the initiating document's: `msg.sourceUrl` for the SSRF classification
  // and `msg.origin` for the credentialed read, so a same-origin child carries the session as this document's
  // own load did. `unavailable` has no reader here (a child's page is not in the popup's page-source row).
  /* `provenance` is the engine's statement of who named the address, carried by every record reaching this
     function (the `document.fetch` request, the `navigable.create` and `navigable.swap` notices), relayed
     and never re-derived: an address cannot tell a frame the person's session would load from one that exists
     because a gate was forced. */
  /* `refusalArm` is the consumer's, not this function's: `document.fetch` is answered through
     engineDeliverDocument, which routes a refusal to the rendezvous the engine is parked on, while
     `navigable.create` and `navigable.swap` are notices with no id to refuse and state `none`. A shared
     literal would let a notice turn a declined load into an empty document, a plausible datum for a frame
     the chokepoint refused. */
  /* This forward serves only `rendezvous` and `none`: no consumer of it drops a work item, so a `report` would
     be honoured by the loader and then fabricated into a document by the notice that asked. A consumer that
     gains such an arm widens this list deliberately. */
  const fetchedDocument = async (u, provenance, driven, pinned, refusalArm, request, readerOpaque, doc) => {
    DCHECK(refusalArm === "rendezvous" || refusalArm === "none",
           "a §7.4 document load was asked for stating the refusal arm " + JSON.stringify(refusalArm) +
           ", and this forward serves only `rendezvous` and `none` — a request that parks on a rendezvous " +
           "and two notices that can state nothing. `report` is a word for a caller that drops a WORK ITEM, " +
           "which none of these three does, so honouring one here would hand a declined record to a consumer " +
           "whose only arm for it fabricates an empty document — which is the literal this parameter " +
           "replaced");
    /* `doc` is the issuing document's record: the creator's for a create, the asker's for a load, the
       issuer's for a swap or a navigation (askerRecord). */
    DCHECK(docRecordIs(doc), "a §7.4 document load was asked for with no issuing document's record");
    const r = await navigationLoad(u, msg.sourceUrl, msg.origin, provenance, doc,
                                   refusalArm, /*actor*/"page", driven, pinned, request,
                                   documentSpeaker(msg, "engine:" + eng.docId),
                                   readerOpaque);
    /* `declined` crosses (engineDeliverDocument routes it to the rendezvous, so the navigable keeps the
       `about:blank` it was created holding); `unavailable` does not, since nothing here reads it. */
    DCHECK(refusalArm === "rendezvous" || r.declined === null,
           "a §7.4 document load came back DECLINED for a consumer whose refusal arm is `" +
           String(refusalArm) + "`" +
           " —" + " `navigationLoad` is supposed to have aborted instead, so either its " +
           "refusal-arm test or this forward has come apart. The arms are not interchangeable: a notice " +
           "has no rendezvous, and the only thing such a consumer could do with a refusal is turn it into bytes");
    DCHECK(r.declined === null || typeof r.declined === "string",
           "a §7.4 navigation's result does not STATE whether it was declined — `r.declined` is `" +
           String(r.declined) + "`. The field used to be read as `r.declined || null`, and that default is the " +
           "whole of what this check replaces: a load arm that forgot to state it read as a load this zone " +
           "fetched, so a frame the chokepoint had refused would be answered with its bytes-less reply through " +
           "the ANSWER path and the engine would park on a rendezvous nothing would ever refuse");
    return { url: r.url, headers: r.headers, bytes: r.bytes, declined: r.declined, setCookies: r.setCookies };
  };
  /* The routing table is the pool. `docId` (which document this instance holds) and `origin` (the value
     stamped on everything it sends) are read only by the notice router below: SECURITY.md makes this zone
     the one that knows which instance holds which document and the one that may state a sender's origin. */
  /* The XMLHttpRequest fetch, engineFetchXhr over this instance. */
  const fetchedXhr = (rec, doc, signal) => engineFetchXhr(eng, rec, doc, signal);
  /* The principal is browser-stated, never parsed off the address: a page that sandboxes its own iframe gives
     that document an opaque origin while its address looks ordinary. _dispatchDocument hands over
     `_senderOrigin(sender)` (from MessageSender) as `msg.origin`, and hostNotice stamps it on every message the
     document posts. A document this zone provisioned carries the origin of the URL this zone fetched; "" is a
     rehydrated recipe predating the field, and the stamp site refuses it. */
  /* `_resolvers` is a list because one document may be asked for more than once while one instance holds it
     (the RESHIP re-delivery); the one engine's finalize answers every waiting caller. */
  /* `cluster` is this instance's identity and `docId` only the document it was rooted at: an instance is an
     origin-keyed agent cluster holding one realm per same-origin document (main.c's `engine_child_realm`).
     `groupId` is kept beside the key because a document this instance creates inherits its group. */
  /* `_remoteAsked` holds the request ids of cross-agent operations already carried to the peer that holds the
     object. It is per instance because an unanswered request is re-reported by qjs_host_requests every step,
     and asking again would run the peer's operation, with the page's side effects, once per step. */
  /* `r` is the instance. A pointer an ABI call retained lives as long as the module, which dies with the frame
     removed at finalize, so nothing in this realm is freed by hand or can read the instance's memory. */
  eng.lines = lines; eng.fkey = fkey; eng.prior = prior; eng.persist = persist;
  eng.fetched = fetched; eng.fetchedDocument = fetchedDocument; eng.fetchedXhr = fetchedXhr;
  eng.code = code; eng.html = html; eng._forceparkSteps = _forceparkSteps;
  /* The document's reach grade, asserted at the one door every instance passes, because the three closures
     above state it on every request; failing here names the composer (ambient seed, declared route, child
     navigable, swapped-to document, cold rehydration) rather than the chokepoint at whichever park runs
     first. It applies to runs that persist nothing too, since their requests still fire by it. */
  DCHECK(msg.provenance === PROVENANCE_OBSERVED || msg.provenance === PROVENANCE_DERIVED ||
         msg.provenance === PROVENANCE_FORCED,
         "a document is being rooted with the reach grade `" + String(msg.provenance) + "`, which is none of " +
         "CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three — every composer of an AST_ANALYZE states one " +
         "(the ambient seed `observed`, a declared route the join of the declaring document's grade and its " +
         "own word, a child navigable and a swapped-to document the same join, a rehydration the word it was " +
         "parked under), so an absent one is a SIXTH composer nobody told, and every request this document " +
         "makes would be refused at the chokepoint under a word it never stated");
  DCHECK(eng.msg === msg,
         "the reservation this instance is being rooted into carries a different message record than the one " +
         "this document was parsed, fetched and keyed from — the reservation's is what hostNotice stamps a " +
         "cross-document delivery's origin from and what finish() writes the cold recipe out of, so two " +
         "records for one document is a message stamped with one document's principal and persisted under " +
         "another's address");
  /* The first round's record, taken before this instance is rankable: `qjs_begin` above seeded the frontier, so
     a top-flow weight now exists, and the facts are written before the state that makes them readable, so the
     ranking and RAM floor never see a hot engine that has not reported. Its site is asked first. */
  await level1AskSites([eng.msg.sourceUrl]);
  /* A refused first reply leaves nothing to rank, so the boot fails with the refusal as its cause. */
  const refused = await engineRecordFacts(eng);
  if (refused !== null) throw engineBootRefusal(refused);
  eng.state = "hot";
}
/* A second document of an agent cluster joins the instance already running it, with the same arguments
   `qjs_init` takes. It adds a Lexbor tree in the agent's arenas, a realm through main.c's engine_child_realm,
   and that document's scripts seeded on the one frontier; a second instance would be the two-heap split
   SECURITY.md's (browsing-context group, origin) key forbids.
   The instance must already be rooted and seeded (`qjs_join` asserts `g_dom` and `g_begun`), or the document
   would never run a line; a caller holding a reservation waits for it, and this asserts the state.
   The same document may not arrive twice: the engine interns documents by name (main.c's `world_doc_hosted`
   assert), and two spellings of one document would pass that, so the name is asserted unheld across the
   whole pool. */
/* The document's bytes are a parameter, not a field of `msg`: a peer-announced child arrives on a message this
   zone built with `pageHtml`, while a tab navigation arrives on the ambient AST_ANALYZE message, which asserts
   `pageHtml === undefined` (a seed is an address), with its bytes on the waiting job. As a parameter the
   operation takes its input with it, the assert stands on what the caller said, and the shape matches
   engineCreate's. */
async function engineJoin(eng, html, msg, docName, topLevelUrl, inherited, parentNavigable, openerNavigable,
                          containerPolicy,
                          ancestorOrigins, creationSandboxFlags) {
  DCHECK(eng.state === "hot" || eng.state === "fetching",
         "a document was joined to an instance in state `" + eng.state + "` — a join ADDS a document to a LIVE " +
         "agent, and qjs_join's own asserts name the two calls that must already have happened (qjs_init roots " +
         "the agent, qjs_begin seeds the frontier its scripts become members of)");
  DCHECK(typeof docName === "string" && docName !== "",
         "a document was joined with no NAME — a peer routes a delivery and a cross-agent operation on that " +
         "name, so an unnamed one is a document no message can ever reach");
  DCHECK(hostHolderOf(docName) === null,
         "a document was joined under a name some instance in this pool already holds — the engine interns " +
         "documents by name, so this would either be one document arriving twice (a second tree, a second " +
         "realm and a second run of its scripts in one agent) or two documents wearing one name, and both are " +
         "a routing table that answers a peer's delivery with whichever it wrote last");
  DCHECK(msg.sourceUrl !== "" && originOf(msg.sourceUrl) === eng.origin,
         "a document whose principal is not this instance's was joined to it — an instance is an ORIGIN-KEYED " +
         "agent cluster and SECURITY.md makes the instance the principal, so this would put two origins behind " +
         "one credentialed-read principal; the engine's own CHECK aborts on it, and this zone computed the " +
         "cluster key that sent it here");
  /* The parent navigable, on engineCreate's rule. A same-origin document nested through a cross-origin one
     belongs to this cluster and to another instance's frame tree at once, and only this zone can name its
     parent. */
  DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
         "a document was joined with no HTML §7.3.1.3 PARENT NAVIGABLE — a navigable either has a parent or " +
         "is a top-level traversable and both are facts a host states, so an absent one is a caller that " +
         "skipped the question and a nested document that would answer `parent === self` inside the only " +
         "instance that holds it");
  DCHECK(typeof openerNavigable === "string" && openerNavigable !== "",
         "a document was joined with no HTML §7.2.2.4 OPENER NAVIGABLE — a create notice carries the identity " +
         "its engine wrote and every other call site states `N`, so an absent one is a caller that skipped it");
  /* The container answer, on engineCreate's rule: for such a document both HTML §7.3.1.3 links are in another
     instance, so only that instance could compute Permissions Policy §9.5's answer. */
  DCHECK(typeof containerPolicy === "string" && containerPolicy !== "",
         "a document was joined with no HTML §7.3.1.3 CONTAINER statement — §9.5's answer for its navigable " +
         "and `null` are the two facts a host states, so an absent one is a caller that skipped the question " +
         "and a nested document granted every feature its embedder holds, through §9.7 step 1's null-container "+
         "arm");
  /* The ancestor origins, on engineCreate's rule: every ancestor of such a document is in another instance,
     which alone can read HTML §3.1.3's inputs. */
  DCHECK(typeof ancestorOrigins === "string" && ancestorOrigins !== "",
         "a document was joined with no HTML §3.1.3 ANCESTOR ORIGINS statement — the composed list and " +
         "`none` are the two facts a host states, so an absent one is a caller that skipped the question and " +
         "a nested document that would answer `location.ancestorOrigins` with `[]` while sitting inside " +
         "somebody else's frame tree");
  /* The creation sandboxing flags, on the same rule. A join is reachable with a sandbox: `<iframe
     sandbox="allow-same-origin allow-scripts" src="https://cdn/…">` keeps the child's tuple origin (so it
     clusters with that host's documents) while most HTML §7.1.5 Sandboxing flags stay set. The sandboxed
     origin flag cannot arrive here: principalOf gives such a document an opaque origin that clusters with
     nothing, and main.c's join entry asserts it. */
  DCHECK(typeof creationSandboxFlags === "string" && creationSandboxFlags !== "",
         "a document was joined with no HTML §7.1.5 CREATION SANDBOXING FLAG SET — the composed set and " +
         "`none` are the two facts a host states, so an absent one is a caller that skipped the question and " +
         "a sandboxed frame joined with its scripts, its forms and its `document.domain` unsandboxed");
  /* The same shape the root's document takes: `content.mojom.Renderer.Join` declares `array<uint8>` because
     `qjs_join` has main.c's byte-identical signature, so both operations take one contract. */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a document was joined with no BYTES — every live document is navigationLoad's response body and a " +
         "cold-tier one may hold characters, so anything else is a document this agent would hold as nothing " +
         "at all. It is the CALLER'S to state: a peer engine's create notice carries it on the message this " +
         "zone built, and a tab navigation carries it on the waiting job (`_waiting`'s `html`), never on the " +
         "AST_ANALYZE message — whose own entry asserts `pageHtml === undefined` because a seed is an address");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* A NUL in a joined document is input, as for the root: `qjs_join` takes `(bytes, len)` and the tokenizer
     applies its per-state rule (HTML §13.2.3.5 Preprocessing the input stream). */
  /* The clone of the creator's container, off the create notice that sent this document. The pair is empty
     where there is no creator: a cross-document navigation of a top-level traversable (HTML §7.4.6.1
     Updating the traversable) joins a Document whose container comes from its own response. There is no
     `null` arm: engineRoot takes one for a rehydrated recipe with no message, and every caller here has one. */
  DCHECK(!!inherited && typeof inherited.csp === "string" && typeof inherited.cspReportOnly === "string" &&
         typeof inherited.selfOrigin === "string" &&
         (inherited.selfOrigin !== "" || (inherited.csp === "" && inherited.cspReportOnly === "")) &&
         embedderPolicyWhole(inherited.embedder) &&
         _REFERRER_POLICIES.includes(inherited.referrerPolicy),
         "a document was joined with a CSP list and no SELF-ORIGIN for it — CSP §2.2 \"Policies\" makes a list " +
         "a struct of policies AND the origin `'self'` resolves against, so a policy with no self-origin is " +
         "half a container and §6.7.2.8 would answer one directive backwards. BOTH EMPTY IS A REAL STATE AND " +
         "THIS ASSERT USED TO REFUSE IT: it demanded a creator on the ground that `every join in this zone " +
         "comes off a navigable.create`, and that stopped being true the moment a CROSS-DOCUMENT NAVIGATION " +
         "of a top-level traversable joined its incoming Document here (HTML §7.4.6.1 \"Updating the " +
         "traversable\"). That Document has NO creator — HTML §7.1.7 \"Policy containers\" clones a creator's " +
         "container only where there is one, and this one's is created from its OWN response — so the empty " +
         "pair is the positive statement the engine already reads it as, and demanding a creator would have " +
         "made this zone invent one");
  const _join = await eng.r.renderer.join({
    document: _doc, url: msg.sourceUrl, docId: docName,
    headers: responseFieldLines(msg.responseHeaders), topLevelUrl,
    inheritedCsp: inherited.csp, inheritedCspReportOnly: inherited.cspReportOnly,
    inheritedCspSelfOrigin: inherited.selfOrigin,
    inheritedCoep: inherited.embedder.value, inheritedCoepEndpoint: inherited.embedder.endpoint,
    inheritedCoepReportOnly: inherited.embedder.reportOnlyValue,
    inheritedCoepReportOnlyEndpoint: inherited.embedder.reportOnlyEndpoint,
    inheritedReferrerPolicy: inherited.referrerPolicy,
    parentNavigable, openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags,
    arrival: msg.arrival });
  /* As qjs_init's: a non-zero return is a contract this zone does not speak, so the instance is refused and
     the join goes no further; the instance is finalized at its next step. */
  if (_join.rc !== 0) {
    if (!eng._crashed) engineRefuse(eng, "qjs_join answered rc " + JSON.stringify(_join.rc) + ", which this zone has no handling for");
    return;
  }
  /* A joined document's open referrer policy entry, read before any step, as engineRoot reads a rooted one. */
  DCHECK(typeof msg.seedUrl !== "string" || String(msg.documentId) === docName,
         "a live seed was joined under a name that is not its browser documentId — the navigation arm joins it as " +
         "the group's new top under that name, and the seed's referrer policy entry is keyed by it");
  if (_documentReferrerPolicies.has(docName) &&
      enginePolicySettle(eng, docName, (await eng.r.renderer.getReferrerPolicy({ docId: docName })).referrerPolicy)
        !== null) return;
  /* The name enters the routing table only once the engine holds it; a delivery in between is refused by the
     post branch's assert rather than routed to an agent that has never heard the name. */
  eng.joinedDocIds.push(docName);
  _provisioned.set(docName, { groupId: eng.groupId, ended: null });
  /* The joined document's record, written once. */
  CHECK(!eng.records.has(docName) && eng.records.size > 0,
        "a joined document's record was written twice, or into an instance whose root has none");
  eng.records.set(docName, docRecordOfArrival(msg.arrival, msg.sandbox, msg.addressSpace));
  eng.openers.set(docName, { opener: docRecordNavigableIdentity(openerNavigable), refused: false });
  CHECK(creationSandboxFlags === docRecordSandbox(eng.records.get(docName)),
        "a document is being joined with the flag set `" + String(creationSandboxFlags) + "` beside a record " +
        "whose set is `" + docRecordSandbox(eng.records.get(docName)) + "` — the engine is handed the record's set");
  /* The round ends here, so it records: a join copies a whole document in and seeds its scripts as flows, so
     both Level-1 facts moved. */
  await engineRecordFacts(eng);
}
/* The document a navigation replaced: HTML §7.4.6.1 Updating the traversable deactivates the old document for
   a cross-document navigation, the other half of engineJoin. Both halves are told to the instance in the
   standard's order (the incoming Document exists first). It is not §7.3.1.6 Navigable destruction: the
   navigable survives and only the active Document changes; the two meet at HTML §7.5.9 Unloading documents
   ("if oldDocument's salvageable state is false, then destroy oldDocument") and §7.5.10 Destroying documents,
   which is why the engine serves both entries from one machine.
   Only this zone knows the navigation happened, and only the browser's document id crosses. The call seeds and
   does not wait: the engine attaches the unload to every live timeline, each fires that document's `pagehide`
   and `unload` listeners under its own delta when next scheduled (an unload `sendBeacon` is an endpoint), and
   nothing is dropped. */
/* An instance's frame is removed, and every document it held ends with it (HTML §7.5.10 "Destroying
   documents"): their speak-as rules go (lib/speak-as.js). */
function engineFrameDestroy(eng) {
  for (const name of [eng.docId].concat(eng.joinedDocIds)) if (name !== null) speakAsDocumentEnded("engine:" + name);
  eng.r.destroy();
}
async function engineUnload(eng, docName, incomingDocName) {
  DCHECK(eng.state === "hot" || eng.state === "fetching",
         "a document was reported replaced to an instance in state `" + eng.state + "` — §7.5.9's unload is a " +
         "task of every timeline of that document, so an instance whose frontier is not yet seeded has none " +
         "to attach it to and the destruction would be queued onto nothing");
  DCHECK(typeof docName === "string" && docName !== "",
         "a document was reported replaced with no NAME — an instance is an ORIGIN-KEYED AGENT CLUSTER and " +
         "holds one realm per same-origin document, so the name is the whole of what says which of them the " +
         "browser navigated away from");
  DCHECK(eng.docId === docName || eng.joinedDocIds.indexOf(docName) >= 0,
         "a document this instance never held was reported replaced (" + docName + ") — this zone is what " +
         "routes a document to an instance and what records which documents each one holds, so a name that " +
         "is in neither `docId` nor `joinedDocIds` is this table and the agent disagreeing about what is in " +
         "the agent, and the engine's own `world_doc_hosted` assert is what it would reach");
  /* The order of the two halves is checked here, by the party that performed the navigation. The engine does
     not check it and must not: a cross-origin incoming Document loads into a peer instance, so the agent asked
     to unload never holds it. The engine takes only the outgoing name, the realm HTML §7.5.9 Unloading
     documents queues the operation in. */
  DCHECK(typeof incomingDocName === "string" && incomingDocName !== "" && incomingDocName !== docName,
         "a navigation was reported with no INCOMING document, or with one document named as both halves — " +
         "HTML §7.4.6.1 replaces a navigable's active document WITH ANOTHER, so a report with only an " +
         "outgoing half is a destruction wearing a navigation's name, and one naming a single document as " +
         "both halves is a navigation that did not happen");
  DCHECK(eng.joinedDocIds.indexOf(incomingDocName) >= 0,
         "a navigation named an INCOMING document this instance has not joined (" + incomingDocName + ") — " +
         "engineJoin is what puts it in the agent and is what pushes the name here, so this is the two halves " +
         "of one navigation called in the wrong order");
  /* The name stays in `joinedDocIds`: HTML §7.5.10 destroys the document, not the realm, so the engine still
     interns the name and a peer's route must resolve here to an instance that reports a destroyed navigable
     (what HTML §7.2.2.1's `closed` reads). Removing it would let the next arrival re-join that name and hit
     the engine's one-realm-per-document CHECK. */
  await eng.r.renderer.unload({ docId: docName });
  speakAsDocumentEnded("engine:" + docName);
  /* The round ends here, so it records: an unload attaches a job to every live timeline. */
  await engineRecordFacts(eng);
}
/* CSP §4.1.2 "Should request be blocked by Content Security Policy?" for a request to `url` the document
   `documentId` would make with the empty destination (a fetch()), answered by the instance holding that
   document (content.mojom.Renderer.CheckRequestPolicy) for its answer alone. Answers {verdict, directive}:
   `allowed`, or `blocked` or `report-only` with the directive that refused it; `unknown` with "" when no
   running instance holds the document, so a surface never shows an unasked policy as allowing; or `refused`
   with "" and the `reason` when the instance's answer is none of its forms. The answer is the instance's
   statement and so input: it is refused through engineRefuse, which retires the instance, and is never read
   as allowing. The verdict gates no send (the Send panel shows it beside the person's own act), so a refusal
   declines nothing.
   Not covered: a document whose instance is booting or parked cold, which answers `unknown`. The next diff
   waits for a booting instance's `_readyP` and resumes a cold one through its recipe. Its absence shows as
   `unknown` for a frame whose document this zone has analysed but is not running now. */
async function engineRequestPolicyVerdict(documentId, url) {
  DCHECK(typeof documentId === "string" && documentId !== "" && typeof url === "string" && URL.canParse(url),
         "a request policy was asked with no document name or an unparseable URL — the Send panel states both");
  const eng = hostHolderOf(documentId);
  /* A retired instance is asked nothing more, as `step` and engineFinalize ask it nothing. */
  if (eng === null || eng._crashed || (eng.state !== "hot" && eng.state !== "fetching"))
    return { verdict: "unknown", directive: "" };
  const r = await eng.r.renderer.checkRequestPolicy({ docId: documentId, url: new URL(url).href, destination: "" });
  const parts = typeof r.verdict === "string" ? r.verdict.split("\t") : null;
  if (parts !== null && parts.length === 1 && parts[0] === "allowed") return { verdict: "allowed", directive: "" };
  if (parts !== null && parts.length === 2 && (parts[0] === "blocked" || parts[0] === "report-only") &&
      parts[1] !== "")
    return { verdict: parts[0], directive: parts[1] };
  const why = "CheckRequestPolicy answered " + JSON.stringify(r.verdict) + ", which is neither `allowed` nor " +
              "`blocked` or `report-only` naming its directive";
  /* The instance may have a round in flight; it is finalized at its next step, which skips a crashed one. */
  if (!eng._crashed) engineRefuse(eng, why);
  return { verdict: "refused", directive: "", reason: why };
}
/* What engineRecordFacts answers for an instance it does not record because it was retired. */
const ENGINE_RETIRED = "the instance was retired before this round's facts could be recorded";
/* The Level-1 ranking's facts (the instance's account and its working set), recorded at the end of every round
   this zone has with an instance (seed, step, service), because an instance behind the frame boundary answers
   only by message and there is no value to read on the line that ranks. hostSchedule ranks, advances the
   winner and re-ranks, so the engine that just worked has the freshest numbers and one that did none has
   numbers that cannot have moved. qjs_level1_account DCHECKs `g_begun`, which qjs_begin sets.
   The reply is the instance's statement and so input: one that fails level1AccountRefusal is refused through
   engineRefuse and nothing is recorded, so the instance keeps the facts its last accepted reply stated. Answers
   the refusal, ENGINE_RETIRED, or null for an accepted reply.
   A crashed instance (refused or aborted) is never recorded, whichever op retired it and in whatever order its
   answers arrive: it is not asked, and a retirement landing while the reply is in flight (a policy refusal
   beside a step or service round) discards the reply. */
async function engineRecordFacts(eng) {
  if (eng._crashed) return ENGINE_RETIRED;
  const _rank = await eng.r.renderer.getLevel1Account();
  if (eng._crashed) return ENGINE_RETIRED;
  let a;
  try { a = JSON.parse(_rank.account); } catch (_) { a = undefined; }
  const why = level1AccountRefusal(_rank.account, a, _rank.workingSetBytes, eng.level1);
  if (why !== null) { engineRefuse(eng, why); return why; }
  /* The instance's lifetime counters are kept to difference the next reading against; what ranks is the
     discounted account they credit, at the product's size now. */
  const was = eng.level1 === undefined ? { paid: 0, pulls: 0, acct: LEVEL1_UNSERVED } : eng.level1;
  eng.level1 = { runnable: a.runnable === 1, paid: a.paid, pulls: a.pulls,
                 acct: level1Credit(was.acct, a.paid - was.paid, a.pulls - was.pulls, level1ProductSize()) };
  /* The working set as the frame stated it on the call above: `workingSetBytes` is a declared reply field of
     every `content.mojom.Renderer` method, and only a call can grow the memory, so the reply just awaited is
     the freshest value. */
  eng.residentBytes = _rank.workingSetBytes;
  return null;
}
/* The Level-1 reply's contract, or the first field that breaks it. main.c's qjs_level1_account states
   `runnable` (0 or 1) and the lifetime counters `paid` and `pulls`, which only climb within one instance (`last`
   is the instance's previous accepted reading, or undefined); a ranked account's shape is not this reply. The
   frame states its linear memory's length, which WASM allocates and grows in 64 KiB pages. */
function level1AccountRefusal(raw, a, bytes, last) {
  const f = "the instance's Level-1 account (" + JSON.stringify(raw) + ")";
  const why = docObject(a, f);
  if (why !== null) return why;
  if (a.runnable !== 0 && a.runnable !== 1) return f + " states `runnable` neither 0 nor 1";
  for (const k of ["paid", "pulls"])
    if (!docIsCount(a[k]) || a[k] < 0) return f + " states no finite non-negative `" + k + "`";
  if (last !== undefined && (a.paid < last.paid || a.pulls < last.pulls))
    return f + " fell below the instance's last reading (paid " + last.paid + ", pulls " + last.pulls + ")";
  return Number.isInteger(bytes) && bytes > 0 && bytes % 65536 === 0 ? null
    : "the instance's working set (" + String(bytes) + ") is not a whole number of 64 KiB WASM pages";
}
/* The document's account: the stored row this instance resumed (engineRoot's `prior`) plus the instance's own
   lifetime counters, so a document's service survives a park. */
function engineAccount(eng) {
  const p = eng.prior;
  return level1Sum(p ? p : LEVEL1_UNSERVED, eng.level1.acct, level1ProductSize());
}
/* The Level-1 key of an instance that has reported (level1Key over its address and account): last at both levels
   when no member can take the thread, so its next step retires it through DONE or its service round pays it. A
   reservation or a loading seat has reported nothing and is never ranked. */
function engineKey(eng, tree) {
  DCHECK(eng.state === "hot" || eng.state === "fetching",
         "the Level-1 ranking was asked for the key of an engine in state `" + eng.state + "` — only an " +
         "instance that has stated its account can be ranked");
  DCHECK(eng.level1 !== undefined,
         "an engine that has been rooted carries no Level-1 account — engineRecordFacts writes one before the " +
         "record first becomes hot (a refused first reply fails the boot) and keeps it on every later round");
  return level1Key(tree, eng.msg.sourceUrl, engineAccount(eng), eng.level1.runnable, LEVEL1_DOOR.instance);
}
/* One delivery, both channels, so no call site carries the record and forgets the bytes. `rep` is what
   `fetched` answered: `null` for a network error (the JSON `null` the engine rejects with a TypeError), or
   `{meta, bytes}`. The renderer places the bytes in linear memory (one extra byte so an empty body still has
   an address; `null` means no body) and frees them after the call, as the mojom declares the body not
   retained.
   The bytes are copied, not transferred: mojo.js builds a transfer list from declared types, Mojo's
   `array<uint8>` is a copied byte sequence, and a moved buffer would be `mojo_base.mojom.BigBuffer`. A move is
   therefore a second declared type, never a flag on this call, because `Init`'s document has the same
   spelling and its bytes this zone retains. */
/* Delivered against the request, which is the pair: `qjs_provide` takes the method first, as a request line
   states it (RFC 9112 §3 Request Line), because the engine's pending register is keyed on both, so a GET and a
   POST to one address cannot collect each other's bodies. */
async function engineProvide(eng, method, mode, referrerPolicy, referrer, url, rep) {
  /* A decline may not arrive here: `provide` settles the park, and `reply: "null"` settles it with a network
     error. A decline is delivered through engineDecline, which keeps the flow parked. */
  DCHECK(!(rep && rep.refusal),
         "a DECLINED request reached the delivery seam — `provide` settles the park, and settling it with a " +
         "network error tells the flow the server was unreachable for a request this zone chose not to send. " +
         "A decline is delivered by NOT calling this: the flow stays parked, which is what §@S requires of a " +
         "search not yet solved, and it fires the day the origin is widened. Refusal: " +
         String(rep && rep.refusal && rep.refusal.reason));
  DCHECK(rep === null || (rep && typeof rep === "object" && rep.meta && rep.bytes instanceof Uint8Array),
         "`fetched` answered neither §5.6's network error (null) nor a reply — a reply is its JSON metadata " +
         "and its BYTES together, and a record arriving without one of the two is half a response");
  await eng.r.renderer.provide({ method, mode, referrerPolicy, referrer, url,
                                 reply: JSON.stringify(rep === null ? null : rep.meta),
                                 body: rep === null ? null : rep.bytes });
}
/* The other answer on the same pair: this zone stating it will not make the request. It is not a delivery
   with a different payload, which is why engineProvide refuses a refusal. The engine keeps the flow's park and
   forks an arm that runs the page's `catch`, so the wait is preserved and the failure path explored. No bytes
   accompany it, since nothing was fetched. */
async function engineDecline(eng, method, mode, referrerPolicy, referrer, url, reason) {
  DCHECK(typeof method === "string" && method !== "" && typeof mode === "string" && mode !== "" &&
         typeof referrerPolicy === "string" && typeof referrer === "string" && referrer !== "" &&
         typeof url === "string" && url !== "",
         "a refusal was relayed naming no request — it is keyed on the line's five-field identity exactly as " +
         "a reply is, because it answers the same question, and a partly-named one refuses a request no flow " +
         "parked on");
  DCHECK(typeof reason === "string" && reason !== "",
         "a refusal was relayed with no reason — the flow it refuses will not drain this session, so this is " +
         "the only account of it anybody gets and it is what says whether a widening would change the answer");
  await eng.r.renderer.decline({ method, mode, referrerPolicy, referrer, url, reason });
}
/* The engine's xhr.send record (xml_http_request.c's `xhr_request_op`), or the first field that breaks it.
   Every field is one the chokepoint decides from: the URL and method (SOP, CORS, the method refusal), XHR
   §3.5.6 The send() method's two credentials modes and its `cors` mode (a reply in another mode would skip the
   CORS check), the referrer policy and referrer the `Referer` is composed from, step 6's use-CORS-preflight
   flag, and solver/engine.h's provenance, witness mark (absent on an older build) and drive mark. */
function xhrRecordRefusal(q, raw) {
  const f = "the engine's xhr.send record (" + JSON.stringify(String(raw).slice(0, 200)) + ")";
  if (!docIsRecord(q)) return f + " is not a JSON object";
  if (!docIsName(q.url)) return f + " names no URL";
  if (!docIsName(q.method)) return f + " names no method";
  if (q.credentials !== "include" && q.credentials !== "same-origin")
    return f + " states the credentials mode " + JSON.stringify(q.credentials) + ", neither include nor same-origin";
  if (q.mode !== "cors") return f + " states the mode " + JSON.stringify(q.mode) + ", not cors";
  if (_REFERRER_POLICIES.indexOf(q.referrerPolicy) < 0)
    return f + " states the referrer policy " + JSON.stringify(q.referrerPolicy) + ", which Referrer Policy §3 does not define";
  if (typeof q.referrer !== "string" || (q.referrer !== "no-referrer" && !URL.canParse(q.referrer)))
    return f + " states the referrer " + JSON.stringify(q.referrer) + ", neither no-referrer nor a URL";
  if (typeof q.useCorsPreflight !== "boolean") return f + " states no use-CORS-preflight flag";
  if (q.provenance !== PROVENANCE_OBSERVED && q.provenance !== PROVENANCE_DERIVED && q.provenance !== PROVENANCE_FORCED)
    return f + " states the provenance " + JSON.stringify(q.provenance) + ", none of observed, derived and forced";
  if ("pinned" in q && q.pinned !== PINNED_YES && q.pinned !== PINNED_NO)
    return f + " states the witness mark " + JSON.stringify(q.pinned) + ", neither pinned nor unpinned";
  if (q.driven !== DRIVEN_YES && q.driven !== DRIVEN_NO)
    return f + " states the drive mark " + JSON.stringify(q.driven) + ", neither driven nor undriven";
  return null;
}
/* The XMLHttpRequest fetch (XHR §3.5.6 The send() method) of instance `eng` through the chokepoint. `rec` is
   the engine's JSON request record (method, url, headers, body); the answer is the reply shape fetch_reply_new
   builds, and a null body is the network error §3.5.6's handle errors turns into an `error` event. It is a
   function of the instance, not a closure of engineRoot, so the refusal of a record is driven in a realm. */
async function engineFetchXhr(eng, rec, doc, signal) {
  DCHECK(signal instanceof AbortSignal,
         "an XMLHttpRequest send reached the fetch relay with no abort signal — engineIssue owns one per send, " +
         "and a `fetch.cancel` notice naming its rendezvous ends the send through it");
  /* The request record is the engine's, crossing as JSON so it carries its types, and so input: a record
     xhrRecordRefusal refuses retires the instance and is never sent. The method arrives normalized (Fetch
     §2.2.1 Methods; XHR §3.5.1 The open() method runs "normalize a method"), so GET, HEAD included, is
     exactly what this zone can perform; another is refused below, never downgraded, because safeFetch is GET
     only (SECURITY.md) and a POST answered with a GET's reply is a response the server never gave. */
  let q;
  try { q = JSON.parse(rec); } catch (_) { q = undefined; }
  const _xhrRefusal = xhrRecordRefusal(q, rec);
  if (_xhrRefusal !== null) {
    if (!eng._crashed) engineRefuse(eng, _xhrRefusal);
    return { declined: _xhrRefusal };
  }
  /* The refusal is the chokepoint's grade (`safeFetchMethodRefusal`), shared with `fetched` and
     engine/trusted.mjs. It is a decline: no browser refuses a POST, so the rendezvous is declined and the
     engine forks a waiting arm and a failure arm, as for a refused document load. */
  const _xhrMethodRefusal = self.safeFetchMethodRefusal(q.method);
  if (_xhrMethodRefusal) return { declined: q.method + " " + q.url + " — " + _xhrMethodRefusal.reason };
  try {
    const abs = new URL(q.url, eng.msg.sourceUrl).href;
    /* The credentials mode (XHR §3.5.6 The send() method: `include` if cross-origin credentials is true,
       otherwise `same-origin`) is relayed as `opts.credentials`, a statement about the request that
       safe-fetch.js composes with `credentialed` and so can refuse and never grant. Fetch §4.10 CORS check
       reads it: an `include` reply passes only with the exact origin and `Access-Control-Allow-Credentials:
       true`. The mode is §3.5.6's own `cors`; safe-fetch.js's `_modeOf` CHECKs the word. The origin is the
       instance's, from this zone's record (see `fetched`). */
    CHECK(typeof eng.origin === "string",
          "an XMLHttpRequest is about to be judged with no origin on the instance that made it — the zone " +
          "assigns every instance its cluster origin at creation");
    /* @security-finding: `headers` come from the untrusted bundle and are read by the chokepoint. That is
       within the model for an uncredentialed GET to a public host (the browser strips forbidden header names),
       and must be re-decided the day this path is credentialed. Only what the chokepoint reads is passed. */
    /* An XMLHttpRequest's destination is the empty string (Fetch §2.2.5 Requests), stated so its CORB class
       is not decided by silence. */
    /* What the request is evidence of (`derived` or `forced`, never `observed`, since no parser inserted an
       XHR) and its witness mark, both composed at the record by `xhr_request_op`. `credentials` is relayed
       and `credentialed: false` is stated as a literal, since the egress surface grades the `cookies` row
       `certain` and the door asserts every caller states one. The witness mark is read with `in`, never
       `||`: an absent key is an older build (this JS ships before the engine is rebuilt), and the default a
       `||` would supply, `unpinned`, is the word safe-fetch.js's value arm fires on, so an absent key is
       answered `unstated`, a word the engine never spells. */
    const _xhrPinned = "pinned" in q ? q.pinned : "unstated";
    /* `doc` is the instance's record, for the reason `fetched` gives. */
    /* `actor: "page"`: every XHR is made by running the page's code, the same population as the `fetch()`
       relay. With a real witness mark, an XHR whose path pinned nothing fires at an origin the person
       permitted for that row, and one whose path pinned a witness is refused, as such a `fetch()` is. */
    /* The drive mark is relayed off the record (`xhr_request_op` writes it beside the grade); the chokepoint
       CHECKs that a `page` request states one. */
    const r = await self.safeFetch(abs, { pageOrigin: eng.origin, destination: "",
                                          mode: q.mode, provenance: q.provenance, pinned: _xhrPinned,
                                          driven: q.driven,
                                          doc, actor: "page",
                                          credentialed: false, useCorsPreflight: q.useCorsPreflight,
                                          credentials: q.credentials, headers: q.headers, signal,
                                          identity: await pageRequestIdentity(stampOrigin(eng.origin), eng.msg.sourceUrl, "engine:" + eng.docId, abs,
                                                                        q.credentials, q.mode,
                                                                        q.referrerPolicy, q.referrer) });
    DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number" &&
           r.headers && typeof r.headers === "object",
           "safeFetch answered an XHR with something other than its reply record — §3.5.6's response is " +
           "built from it and the page reads status, statusText and every header off that, and §2.2.4's " +
           "body source is a BYTE SEQUENCE");
    /* The status message is asserted for `fetched`'s reason: "" is a legitimate HTTP/2 answer and the
       refusal arms carry their reason there. */
    DCHECK(typeof r.statusText === "string",
           "safeFetch answered an XHR with no statusText — XMLHttpRequest §3.6.3 `The statusText getter`'s `statusText` getter is " +
           "read straight off it, and the chokepoint writes one on every path including the refusals whose " +
           "reason it is");
    /* The bytes cross beside the record, for `fetched`'s reason: XHR §3.6.6 Response body decodes the received
       bytes with the final encoding, and `responseType = "arraybuffer"` needs the server's own bytes. */
    /* A decline goes to `hostDecline` (see the delivery in the host-request loop). A network refusal is
       XHR §3.5.6 "The send() method"'s network error: an answer with no body, so xhr_take_reply leaves the
       response a network error and the page's XHR runs its error steps. A status-0 record with an empty
       body would instead be a response, and fire `load`. */
    if (r.refusal) {
      CHECK(r.refusal.kind === "network" || r.refusal.kind === "decline",
            "safeFetch graded an XHR's refusal `" + r.refusal.kind + "`, which is neither word it states");
      if (r.refusal.kind === "decline") return { declined: "GET " + abs + " — " + r.refusal.reason };
      return { meta: { status: 0, statusText: r.statusText, headers: [] }, bytes: null };
    }
    /* The type the chokepoint computed, relayed as `fetched` does: this zone is the only one that may answer
       it (safeFetch saw the body and ran the sniff), and solver/reply_decode.c reads `computedType` rather than
       re-deriving a type. "" is MIME Sniffing §5.1's undefined supplied type, a positive answer. */
    DCHECK(typeof r.computedType === "string",
           "safeFetch answered an XHR with no computed content type — solver/reply_decode.c reads this " +
           "field instead of re-deriving a type from the raw header, so an absent stamp is a producer " +
           "that failed and never a resource whose type is unknown");
    return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers),
                     computedType: r.computedType },
             bytes: r.body };
  } catch (e) {
    /* A thrown fetch is the network error that becomes the page's `error` event (XHR §3.5.6 handle errors);
       an invariant abort is rethrown. No `body` key in the record: `bytes: null` states there are none, and a
       second spelling is what `fetch_reply_set_body` DCHECKs against. */
    RETHROW_FATAL(e);
    return { meta: { status: 0, statusText: "", headers: [] }, bytes: null };
  }
}
/* One pending line, split in one place, mirroring the engine's `engine_pending_split` so the grammar cannot
   drift across hosts. The TAB delimiter is unambiguous: URL Standard §4.4 URL parsing strips every ASCII tab
   or newline from its input first, and a method is a token (Fetch §2.2.1 Methods) whose tchar (RFC 9110 §5.6.2
   Tokens) excludes HTAB. It splits on the TAB rather than counting indexes because the destination field is
   legitimately empty (Fetch §2.2.5 Requests: "" for a `fetch()`).
   The line is the instance's statement and so input: answers the request record, or the reason the line is
   refused, which the caller refuses the instance with. A refused line is never delivered against or issued,
   since a mis-split line would key a reply on a request nothing parked on. */
function pendingRequest(line) {
  const f = line.split("\t");
  const why = pendingLineRefusal(f);
  if (why !== null) return "content.mojom.Renderer.GetPending answered `" + line + "`, whose " + why;
  /* The destination (Fetch §2.2.5 Requests) is what this zone acts on: safe-fetch.js decides the CORB class
     with §2.2.5's script-like predicate over it, and its `_destinationOf` CHECKs the enumeration where the
     decision is made, as `_credentialedOf` does for the credentials mode (Fetch §2.2.5 Requests: only the
     algorithm that created the request knows it, so it is relayed, never re-derived). The initiator is HTML
     §4.12.1.1 Processing model's parser-inserted; nothing here branches on it. The provenance, witness mark
     and drive mark are composed at the park (solver/engine.h) and relayed; the firing decision is
     safe-fetch.js's. The referrer policy is after Fetch §4.1 "Main fetch" gave an empty one the policy
     container's, and the referrer is "no-referrer" or a URL (fetch_referrer_for_wire resolved "client"); both
     are relayed for the `Referer` this zone composes. The askers are the documents whose parks the line folds;
     the caller checks each names a document of the asking instance. */
  return { method: f[0], destination: f[1], initiator: f[2], provenance: f[3], pinned: f[4], driven: f[5],
           credentials: f[6], mode: f[7], referrerPolicy: f[8], referrer: f[9], askers: f[10].split(","),
           url: f[11] };
}
/* Referrer Policy §3 "Referrer Policies", the empty string included. */
const _REFERRER_POLICIES = ["", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin",
                            "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin",
                            "unsafe-url"];
/* The small fixed vocabularies of a GetPending line, by field index, each solver/engine.h's. An unknown word
   would be answered by whichever arm a consumer wrote first, and for the witness mark that is the firing
   policy's permissive one (`unstated` is safe-fetch.js's word for an act with no mark, never a line's). The
   mode is one of the three whose reader is the asking page: a `navigate` request's bytes become another
   instance's document (fetchedDocument), so stating it here would ask for another origin's bytes with no CORS
   question asked. */
const PENDING_TOKENS = Object.freeze([
  [2, "initiator", ["parser", "script"]],
  [3, "provenance", ["observed", "derived", "forced"]],
  [4, "witness mark", ["pinned", "unpinned"]],
  [5, "drive mark", ["driven", "undriven"]],
  [7, "mode", ["same-origin", "cors", "no-cors"]],
  [8, "referrer policy", _REFERRER_POLICIES]]);
/* The line's shape, or what breaks it, worded to follow "whose". */
function pendingLineRefusal(f) {
  if (!(f.length === 12 && f[0] !== "" && f[7] !== "" && f[9] !== "" && f[10] !== "" && f[11] !== ""))
    return "fields are not METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>DRIVEN<TAB>" +
           "CREDENTIALS<TAB>MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>ASKERS<TAB>URL";
  for (const [k, name, words] of PENDING_TOKENS)
    if (words.indexOf(f[k]) < 0) return name + " `" + f[k] + "` is none of " + words.join(", ");
  if (f[9] !== "no-referrer" && !URL.canParse(f[9])) return "referrer `" + f[9] + "` is neither no-referrer nor a URL";
  const askers = f[10].split(",");
  if (!askers.every((a, n) => a !== "" && askers.indexOf(a) === n))
    return "asker set `" + f[10] + "` is not a comma-joined set of document names";
  return null;
}
/* The door that does not await a body with no end. A round that waited on each pending fetch in turn would let
   a response the server never ends (one `fetch()` or XHR to an endpoint that holds its body open) keep the
   document out of the rankable set for the session. No deadline is used: a timeout would truncate a legitimate
   response into a wrong answer, so requests are issued and delivered on later rounds instead.
   The shape is engine/host/wpt_runner.c's `g_inflight` table (`wpt_issue_pending` issues without waiting;
   `wpt_request_asked` refuses to re-issue a key in flight), and engine/trusted.mjs's `track`/`answered` pair
   is the same table. A record stays until its reply is delivered, not until bytes land: a landed but
   undelivered request is still on the engine's pending list and would otherwise be re-issued next round.
   Two key namespaces, as in wpt_runner.c: a network park is `(method, url)` and a host request is its id,
   kept apart by prefixes, as `cold:` and `seed:` prefix group ids, so an id spelled like a method cannot dedup
   against the wrong request. */
function engineIssue(eng, key, start, deliver) {
  DCHECK(typeof key === "string" && key !== "",
         "a request was issued with no in-flight key — the key is what refuses to re-issue a park the engine " +
         "re-lists every round, so an unkeyed issue is one request per round at somebody's server");
  DCHECK(typeof start === "function" && typeof deliver === "function",
         "a request was issued with no starter or no deliverer — the pair is the whole contract: `start` is " +
         "called exactly once and only when this key is not already in flight, and `deliver` is what a later " +
         "round hands the answer to, so an issue missing either is a request nobody will ever answer");
  if (eng._inflight.has(key)) return 0;
  /* The record enters the map before the call suspends, so the next round, re-listing the same request,
     sees it in flight. `controller` ends the send when the engine says no flow is owed it (engineCancel). */
  const rec = { landed: false, value: undefined, error: null, settled: null, deliver,
                controller: new AbortController(), cancelled: false };
  eng._inflight.set(key, rec);
  /* `settled` never rejects; the error is carried and rethrown at delivery inside the round, so an invariant
     abort out of `eng.fetched` still reaches hostSchedule's failure arm and there is no unhandled rejection. */
  rec.settled = start(rec.controller.signal).then(
    (v) => { rec.landed = true; rec.value = v; },
    (e) => { rec.landed = true; rec.error = e; });
  /* The round can wait on it (hostSchedule's wait arm relies on that). It fails only if `start` threw
     synchronously, which none of the callers can, since each returns an async function's promise. */
  DCHECK(rec.settled && typeof rec.settled.then === "function",
         "a request was issued whose starter did not answer a promise — the round waits on this when the " +
         "frontier has nothing runnable, and a non-promise there resolves the wait immediately and turns the " +
         "stall arm into the full-speed spin it exists to prevent");
  return 1;
}
/* The in-flight key of a request, from the identity its answer is delivered under: the five-field pair of a
   park (`engine_provide`, `engine_decline`), or the rendezvous id of a host request (`engine_host_answer`),
   whichever operation it carries, since one id is one rendezvous. The issue sites and engineCancel both key
   through here, so a cancel names the send it ends. */
function inflightKey(identity) {
  DCHECK(Array.isArray(identity) && (identity.length === 1 || identity.length === 5) &&
         identity.every((x) => typeof x === "string"),
         "a request identity is neither a rendezvous id nor the five-field pair: " + JSON.stringify(identity));
  return (identity.length === 1 ? "req\n" : "fetch\n") + identity.join("\n");
}
/* Fetch §2 Infrastructure's "abort a fetch controller" or "terminate", on the zone's side: the two are one act
   here, since both leave the controller canceled and an abort's error is the page's. The engine's
   `fetch.cancel` notice says no flow is owed the request (solver/engine.c's engine_pending_fetch_cancel for a
   park, engine_host_terminate for a rendezvous). The send ends through its signal (safe-fetch.js disconnects
   the wire's port, and wire.js aborts the platform fetch), and it leaves the in-flight set, so a reply that
   landed meanwhile is never delivered and a later request of the same key is issued anew. A key not in flight
   was cancelled before any round issued it. */
function engineCancel(eng, identity) {
  const key = inflightKey(identity);
  const rec = eng._inflight.get(key);
  if (rec === undefined) return;
  rec.cancelled = true;
  eng._inflight.delete(key);
  rec.controller.abort();
}
/* The delivering half, which the engine's park is waiting for. It returns how many registers it filled,
   progress like `wpt_net_pump`'s return. The record leaves the in-flight set here, not when bytes land, so a
   landed reply not yet handed over still refuses a re-issue. */
async function engineDeliverLanded(eng) {
  let n = 0;
  for (const key of [...eng._inflight.keys()]) {
    if (eng._crashed) return n;
    const rec = eng._inflight.get(key);
    if (!rec.landed) continue;
    eng._inflight.delete(key);
    CHECK(!rec.cancelled, "a cancelled send's reply was about to reach the engine — engineCancel takes a " +
          "cancelled send out of the in-flight set, so its reply has no record to be delivered through");
    if (rec.error) throw rec.error;
    await rec.deliver(rec.value);
    n++;
  }
  return n;
}
/* One round: issue what the engine newly parked on, act on what it owes, deliver what has landed; the engine
   is rankable again as soon as these finish. The final wait fires only when `qjs_step` answered
   ENGINE_STEP_STALLED (nothing runnable at all) and the round made no progress, so it starves no flow and
   replaces a full-speed spin through `macroYield`. It is unbounded, never a deadline (as `wpt_net_pump(ctx,
   1)`): the waiting document is out of the hot set, so every other engine keeps stepping. */
async function engineServiceFetch(eng, stalled) {   // one round: issue, act, deliver — never await a body
  DCHECK(typeof stalled === "boolean",
         "a service round was driven without the engine's own stall statement — `qjs_step` answers " +
         "ENGINE_STEP_STALLED for a frontier holding nothing runnable, and that is the ONLY state in which " +
         "this round may wait on bytes; inferring it here would be a second answer to a question the engine " +
         "already answered, and defaulting it either spins or freezes");
  let did = await engineIssuePending(eng);
  /* A retirement can land at any await, and a retired instance is asked nothing more. */
  if (eng._crashed) return;
  did += await engineServiceHostRequests(eng);
  if (eng._crashed) return;
  did += await engineDeliverLanded(eng);
  if (eng._crashed) return;
  if (stalled && did === 0 && eng._inflight.size > 0) {
    await Promise.race([...eng._inflight.values()].map((r) => r.settled));
    await engineDeliverLanded(eng);
  }
}
async function engineIssuePending(eng) {   // issue every parked request; a later round delivers the answer
  /* The reply's metadata crosses as JSON text (as qjs_host_answer's does), so it can say `null` and carry the
     URL list, status and headers; its body crosses as bytes beside it, since decoding it (Fetch §5.3 Body
     mixin's `text()`) would destroy what the engine's own decode reads. */
  /* There is one owed list and each line carries its own class: the destination (Fetch §2.2.5 Requests) on the
     pending line decides whether a reply is code, via safe-fetch.js's script-like predicate, so a document's
     own `<script src>` and an injected one are classified like a dynamic `import()`. A fact about a request
     belongs on the request. */
  const requests = owedList("GetPending", (await eng.r.renderer.getPending()).requests);
  if (eng._crashed) return 0;
  /* The provenance travels with its request to safe-fetch.js, the sole owner of the firing decision. */
  /* How many requests this round started, as `wpt_issue_pending` returns: progress, so the round has something
     outstanding to wait on. */
  let issued = 0;
  for (const line of requests) {
    const req = pendingRequest(line);
    /* A refused line retires the instance; nothing more of this list is issued. */
    if (typeof req === "string") { engineRefuse(eng, req); return issued; }
    const { method, destination, provenance, pinned, driven, credentials, mode, referrerPolicy, referrer, askers,
            url } = req;
    /* The ask is counted at the call, before the gate, so the census separates a request nobody made from one
       the gate refused: `declined: {}` beside a nonzero `asked` says the policy refused nothing (an empty
       surface is then about the driving), while `asked: 0` says this loop never ran. */
    /* The dedup key is the five-field identity the engine parked on, the key the answer is delivered under
       (`engine_provide` and `engine_decline` key on it, and `engine_pending_fetches` dedups over it). It uses
       the raw address off the line, never the absolute URL `fetched` resolves. */
    const key = inflightKey([method, mode, referrerPolicy, referrer, url]);
    /* An entry stays listed until answered and this visits the list every slice, so without the dedup one
       page `fetch` would become one request per round. */
    if (eng._inflight.has(key)) continue;
    /* The ask is raised inside the starter, which `engineIssue` invokes only for a key not in flight, so a
       re-listed request is counted once and the containment with the declined histogram holds. */
    issued += engineIssue(eng, key,
      (signal) => { eng._egress.asked++;
                    return eng.fetched(method, url, destination, provenance, pinned, driven, credentials, mode,
                                       referrerPolicy, referrer, askerRecord(eng, askers, "asker"), signal); },
      (answer) => engineDeliverReply(eng, method, mode, referrerPolicy, referrer, url, answer));
  }
  return issued;
}
/* What a landed answer means on the pending seam: the `deliver` half of one issue, closing over the request's
   own pair, so the reply cannot be handed over against a pair it was not fetched under. */
async function engineDeliverReply(eng, method, mode, referrerPolicy, referrer, url, answer) {
  /* A decline is its own delivery, on its own method. `Decline` carries the same pair `Provide` does with the
     chokepoint's reason: the engine records the refusal, stops listing the request (silence would be
     re-offered and re-refused every round), keeps one arm waiting with no reply invented, and forks the other
     to run the page's failure path marked forced, since a declined request's outcome is unobserved and both
     arms are feasible. The reason names either a row of the person's own control that would make it fire
     (`blocked-signal:cookies=yes`) or a refusal nothing reopens (`blocked-destructive:logout`); the address
     is derived in full and reported. */
  if (answer !== null && answer.refusal) {
    DCHECK(typeof answer.refusal.reason === "string" && answer.refusal.reason !== "",
           "a declined request carries no reason — the reason is the only account a person gets of a park " +
           "this zone will not pay, and an unnamed one leaves a frontier stalled with nothing to read");
    DCHECK(answer.refusal.kind === "decline",
           "a refusal graded `" + answer.refusal.kind + "` reached the decline seam — `network` is a " +
           "refusal a real browser performing this same request also makes, so it is Fetch §5.6 \"Fetch " +
           "methods\"' network error and belongs on `Provide` as the JSON `null`; relaying it here would " +
           "leave the flow parked on a failure that IS a fact about the origin");
    /* It is also told to the person: nothing in the extension renders the engine's record, so this line is
       where a person reading a frontier that will not drain learns which rule holds it. */
    console.warn("[bridge] " + method + " " + url + " — " + answer.refusal.reason +
                 ". This zone DECLINED to make the request: the flow stays PARKED rather than being told " +
                 "the server was unreachable, and one arm is forked to explore the page's failure path");
    /* Counted per whole token, never parsed: safe-fetch.js composes `blocked-signal:<name>=<value>` from the
       signal it walked, so the histogram is per signal by construction, and matching `statusText` would be a
       second copy of the policy. The decline family's tokens come from closed sets (`_SIGNALS` and their
       values, `_DESTRUCTIVE`'s words, Fetch §2.2.5's destinations, lib/document-record.js's
       `_ASKER_REFUSALS`), so no address, origin or document name reaches this map.
       The `network` family (refusals a browser also makes, some carrying an origin) is not counted here. A
       first occurrence is inserted, never read through a default. */
    const _tok = answer.refusal.reason;
    if (!(_tok in eng._egress.declined)) eng._egress.declined[_tok] = 0;
    eng._egress.declined[_tok]++;
    await engineDecline(eng, method, mode, referrerPolicy, referrer, url, answer.refusal.reason);
    /* `return`: the decline is the whole delivery for this request. */
    return;
  }
  await engineProvide(eng, method, mode, referrerPolicy, referrer, url, answer);
}
/* Every owed list crosses the same way, newline-joined records or "" for none, and is split here. The caller
   names the mojom method and mojom declares the reply's type, so a NULL cannot become the text "null" and one
   bogus record. */
function owedList(method, s) {
  DCHECK(typeof s === "string",
         "content.mojom.Renderer." + method + " answered with something that is not text — every owed list " +
         "crosses as newline-joined records and an empty list is the empty string");
  return s.split("\n").filter(Boolean);
}

/* The instance holding a document, by exact name. A child's name is prefixed by its creator's
   ("<creator>.<k>") and the creator is exactly the instance that does not hold it, so a prefix match would
   route a message back to its sender. */
/* It answers for every document the instance holds, not only its root: `qjs_join` adds documents to the live
   agent (main.c's `g_joined_ctx`/`g_joined_dom`) and the engine interns every document by name (`qjs_route`'s
   arrival assert is `world_doc_hosted(world_doc_intern(doc))`, solver/engine.c), so the two lists are one
   table on the other side of the boundary. */
/* The address each provisioned child was created at, by name: the fact a later create under the same name is
   checked against. */
const _createdAddress = new Map();
/* A refusal this zone made of something an engine asked, counted on the instance's histogram beside the declined
   requests. The token is from a closed set (lib/document-record.js's `_ASKER_REFUSALS`). */
function engineCountRefusal(eng, tok) {
  if (!(tok in eng._egress.declined)) eng._egress.declined[tok] = 0;
  eng._egress.declined[tok]++;
}
function hostHolderOf(docName) {
  for (const e of _pool) {
    if (e.docId === docName) return e;
    if (e.joinedDocIds.indexOf(docName) >= 0) return e;
  }
  return null;
}

/* The instance of an agent cluster, a different question from the one above: `hostHolderOf` is routing (where
   a named document is), this is admission (which instance a new document belongs in), answered by the
   (group, origin) cluster, so a document the engine created inside that heap answers here before anything has
   named it to this zone. Asking the routing question for admission would put a same-origin pair in two
   heaps. */
function hostClusterOf(key) {
  for (const e of _pool) if (e.cluster === key) return e;
  return null;
}

/* Set-Cookie pairs for an agent cluster whose instance's jar is not open yet, by cluster key: engineRoot hands
   them to the instance it roots, after Init and before Begin (zoneOpenJar). A group's pairs are reclaimed
   when its last document is destroyed (zoneReclaimHeldCookies). */
const _heldCookies = new Map();
/* The tab groups whose tab has closed, and the groups whose held pairs were reclaimed. Neither comes back: a tab
   id is not reused, and every zone-minted group id is fresh. */
const _closedTabGroups = new Set();
const _goneGroups = new Set();

/* RFC 6265bis §5.7's storage, routed. Each [URL, Set-Cookie field] pair of a response this zone fetched for a
   document of group `groupId` belongs to the jar of the agent its URL's origin keys in that group, whichever
   instance asked for the fetch: a redirect hop on another origin writes that origin's jar, and a document
   created in a peer brings its fields to the peer. An instance whose jar is open receives its pairs now; any
   other's are held for its root. The cluster key is clusterKeyOf's, so an opaque-origin instance receives
   none: a sandboxed document's response cookies are its URL's origin's. */
async function zoneRouteSetCookies(groupId, pairs) {
  DCHECK(Array.isArray(pairs) && pairs.every((c) => Array.isArray(c) && c.length === 2 &&
                                                   typeof c[0] === "string" && typeof c[1] === "string"),
         "a fetch answered no [URL, Set-Cookie field] list — safeFetch states one on every reply");
  /* A load that lands after its group was destroyed has no jar to reach: no document of the group remains. */
  if (_goneGroups.has(groupId)) return;
  const byCluster = new Map();
  for (const p of pairs) {
    const key = clusterKeyOf({ groupId, origin: originOf(p[0]) });
    if (!byCluster.has(key)) byCluster.set(key, []);
    byCluster.get(key).push(p);
  }
  for (const [key, ps] of byCluster) {
    const holder = hostClusterOf(key);
    if (holder && holder.cookiesOpen) {
      await holder.r.renderer.receiveCookies({ setCookies: JSON.stringify(ps) });
    } else {
      if (!_heldCookies.has(key)) _heldCookies.set(key, []);
      for (const p of ps) _heldCookies.get(key).push(p);
    }
  }
}

/* engineRoot's hand-off of the pairs held for `eng`'s cluster. The jar is opened first, so a pair routed while
   the hand-off is in flight goes straight to the instance. */
async function zoneOpenJar(eng) {
  DCHECK(eng.cookiesOpen === false, "an instance's jar was opened twice — engineRoot roots an instance once");
  eng.cookiesOpen = true;
  const held = _heldCookies.has(eng.cluster) ? _heldCookies.get(eng.cluster) : [];
  _heldCookies.delete(eng.cluster);
  await eng.r.renderer.receiveCookies({ setCookies: JSON.stringify(held) });
}

/* Whether any document of browsing-context group `groupId` remains. A tab group's browser documents are all
   destroyed when its tab closes; that, not each document's lifetime port, is the signal, because a document in
   the back/forward cache closes its port without being destroyed and its eviction reports nothing. The group's
   other documents are the instances this zone holds for it. */
function zoneGroupLive(groupId) {
  if (_pool.some((e) => e.groupId === groupId)) return true;
  return typeof groupId === "number" && !_closedTabGroups.has(groupId);
}

/* The spec's end of a browsing-context group, applied to what the zone holds for it: once its last document is
   destroyed, no instance of any of its clusters can be rooted, so the pairs held for them are dropped. Asked
   at each departure of one of its documents (a tab closing, an instance leaving the pool). */
function zoneReclaimHeldCookies(groupId) {
  if (zoneGroupLive(groupId)) return;
  _goneGroups.add(groupId);
  const prefix = String(groupId) + "\u0000";
  for (const key of [..._heldCookies.keys()]) if (key.startsWith(prefix)) _heldCookies.delete(key);
}

/* A tab closed: every browser document of its group is destroyed (offscreen-brain.js's _onTabRemoved). */
function zoneTabClosed(tabId) {
  DCHECK(typeof tabId === "number", "a tab close named no tab id");
  _closedTabGroups.add(tabId);
  zoneReclaimHeldCookies(tabId);
}

/* The pairs of a load the instance `eng` asked for: those of its own cluster stay in the answer, which the
   asking flow's jar applies (core/frame/navigable.c), and every other pair is routed to its own cluster. */
async function zoneAnswerSetCookies(eng, pairs) {
  const own = [], other = [];
  for (const p of pairs) (clusterKeyOf({ groupId: eng.groupId, origin: originOf(p[0]) }) === eng.cluster
                          ? own : other).push(p);
  await zoneRouteSetCookies(eng.groupId, other);
  return own;
}

/* The rendezvous for a cross-agent operation, owned by this zone because neither engine can hold it: the
   asking instance's request id is unique only inside that instance, so the token, opaque to both engines
   (main.c states that at both entries), names which instance and call site a completion belongs to.
   It is not deleted at the first completion: the holder performs the operation on every live timeline and
   each completes with its own answer, so one token names N true completions, and relaying each makes
   engine_host_answer's assert on the fork the asking flow still owes reachable. One entry per operation that
   crossed, dropped with the asking instance (engineFinalize). */
const _remoteOps = new Map();   // token -> { asker, req }
let _nextRemoteToken = 1;
/* A new browsing context group (HTML §7.1.3.2's switch creates one), as the only thing this zone needs of it:
   an id no other group has. Minted here, never derived from an engine's word, since a group id decides which
   documents share a heap. */
let _nextSwapGroup = 1;
/* The same for a `noopener` popup, which HTML §7.3.1.7 creates given a null opener and so in a group of its own. */
let _nextNoopenerGroup = 1;
/* The same for a declared route, a top-level traversable in a group of its own (not nested in the declaring
   document, so it may not share its heap). A separate counter from the swap's so log lines stay readable;
   `seed:` prefixes it as `cold:` prefixes the cold tier's, because group ids are compared against tab ids and
   colliding namespaces would be one namespace. */
let _nextSeedGroup = 1;

/* The outbound request vocabulary, named once. The verb is solver/route_seed.h's `ROUTE_SEED_NOTICE`; the
   provenance words are solver/engine.h's `PENDING_PROVENANCE_*`, one vocabulary for every outbound request,
   fetch or navigation, so the firing policy reads one set of words. `observed` is stated by the ambient seed
   (the person's browser performed that navigation); it is unreachable on a `document.seed` record, which that
   arm's own check states. */
const ROUTE_SEED_NOTICE = "document.seed";
const PROVENANCE_OBSERVED = "observed";
const PROVENANCE_DERIVED = "derived";
const PROVENANCE_FORCED = "forced";
/* The drive mark's two words, solver/engine.h's `PENDING_DRIVEN_*`: whose act a request is, a separate field
   from provenance. */
const DRIVEN_YES = "driven";
const DRIVEN_NO = "undriven";
/* The witness mark's two words, solver/engine.h's `PENDING_PINNED_*`: a separate field from provenance
   (provenance says what a reply is worth; this says whether the act may be spent). `unstated` is not here: it
   is safe-fetch.js's word for a record with no mark (its `_PINNED_MARKS` lists all three), and the engine
   never spells it. */
const PINNED_YES = "pinned";
const PINNED_NO = "unpinned";

/* What this zone owes a one-way notice. Each op is an action only this zone can take (SECURITY.md: the
   offscreen alone knows which instance holds which document); ops not listed here are described at their arms.
   `navigable.create`, fields as navigable_notice.h declares them — the engine has named the document and
   handed the page a WindowProxy for it; this provisions (or joins) an instance under that name, loading the
   child's document through the safeFetch chokepoint.
   `navigable.swap` and `navigable.navigate`, that header's provision record — see those arms.
   `windowproxy.post <target> <world> <marks> <targetOrigin> <base64>` — routed verbatim to the instance holding
   <target>, with this engine's origin stamped on it: the untrusted engine may not state its own origin, since
   a forgeable event.origin defeats every origin check in every bundle. */
/* The shapes a notice's fields take, one checker per shape, each answering null or what breaks it. */
const noticeName = (v, name) => (typeof v === "string" && v !== "" ? null : "carries no " + name);
const noticeWord = (v, words, name) =>
  (words.indexOf(v) >= 0 ? null : "states the " + name + " `" + v + "`, none of " + words.join(", "));
const noticeUrl = (v, name) => (typeof v === "string" && URL.canParse(v) ? null : "states the " + name + " `" + v +
                                                                                 "`, which is not a URL");
/* engine_record_head's marks word, `<provenance>,<drive mark>,<witness mark>`, which safe-fetch.js's
   cross-document rule reads. */
function noticeMarks(m, name) {
  const p = String(m).split(",");
  return p.length === 3 && (p[0] === PROVENANCE_DERIVED || p[0] === PROVENANCE_FORCED) &&
         (p[1] === DRIVEN_YES || p[1] === DRIVEN_NO) && (p[2] === PINNED_YES || p[2] === PINNED_NO)
    ? null : "states the " + name + " `" + m + "`, which is not engine_record_head's marks word";
}
/* A world vector, whose head is `<doc>:<session>:<serial>` (solver/world.c's world_serialize), as
   docRecordWorldDocument reads it. */
function noticeWorld(w, name) {
  const head = String(w).split(",")[0], serial = head.lastIndexOf(":");
  const session = serial > 0 ? head.lastIndexOf(":", serial - 1) : -1;
  return session > 0 && /^[0-9]+$/.test(head.slice(session + 1, serial)) && /^[0-9]+$/.test(head.slice(serial + 1))
    ? null : "states the " + name + " `" + w + "`, which does not begin `<doc>:<session>:<serial>`";
}
/* HTML §7.1.5's creation sandboxing flag set as the create notice spells it: core/frame/sandboxing.h's flag
   names comma-joined once each, or the table's word for the empty set. */
function noticeSandbox(v) {
  const V = NAVIGABLE_NOTICE.sandbox;
  if (v === V.none) return null;
  const names = String(v).split(",");
  return v !== "" && names.every((n, i) => V.flags.indexOf(n) >= 0 && names.indexOf(n) === i)
    ? null : "states the sandboxing flag set `" + v + "`, which is not HTML §7.1.5's flags once each nor `" + V.none + "`";
}
/* A document's marks as lib/document-record.js mints them: an observed document is one a person named, which no
   drive navigated to and no pin addressed. */
function noticeArrival(provenance, driven, pinned) {
  return noticeWord(provenance, [PROVENANCE_OBSERVED, PROVENANCE_DERIVED, PROVENANCE_FORCED], "provenance") ||
         noticeWord(driven, [DRIVEN_YES, DRIVEN_NO], "drive mark") ||
         noticeWord(pinned, [PINNED_YES, PINNED_NO], "witness mark") ||
         (provenance === PROVENANCE_OBSERVED && (driven !== DRIVEN_NO || pinned !== PINNED_NO)
           ? "states an observed document with a drive or witness mark" : null);
}
/* Every field each notice's arm reads, by the op's table (lib/navigable-notice.js, which noticegen.mjs writes from
   navigable_notice.h) or its documented record; null for an op hostNotice does not act on, which its own DFAIL
   names. A notice is the instance's statement, so hostNotice refuses the instance with the reason. */
function noticeRefusal(f) {
  const R = NAVIGABLE_NOTICE.request, C = NAVIGABLE_NOTICE.create, P = NAVIGABLE_NOTICE.provision;
  const parts = (n, exact) => ((exact ? f.length === n : f.length >= n) ? null
    : "has " + f.length + " field(s) where its record has " + (exact ? "" : "at least ") + n);
  const tokenOf = (name) => {
    try { navigableNoticeField(C, f, name); return null; }
    catch (_) { return "carries a `" + name + "` field that is not base64"; }
  };
  switch (f[0]) {
    case "navigable.request":
      return parts(R.parts, true) || noticeName(f[R.creator], "creator") || noticeName(f[R.child], "child") ||
             noticeWord(f[R.destination], NAVIGATION_DESTINATIONS, "destination") ||
             noticeWord(f[R.referrerPolicy], _REFERRER_POLICIES, "referrer policy") || noticeUrl(f[R.address], "address");
    case "navigable.create":
      return parts(C.parts, true) || noticeName(f[C.child], "child") || noticeName(f[C.creator], "creator") ||
             noticeUrl(f[C.address], "address") || noticeName(f[C.topLevelUrl], "top-level creation URL") ||
             noticeWord(f[C.group], ["creator", "new"], "group") ||
             noticeArrival(f[C.provenance], f[C.driven], f[C.pinned]) || noticeSandbox(f[C.sandboxFlags]) ||
             noticeName(f[C.ancestorOrigins], "HTML §3.1.3 ancestor origins statement") ||
             noticeName(f[C.containerPolicy], "HTML §7.3.1.3 container statement") ||
             noticeName(f[C.parent], "HTML §7.3.1.3 parent navigable") ||
             noticeName(f[C.opener], "HTML §7.2.2.4 opener navigable") ||
             noticeName(f[C.cspSelfOrigin], "CSP self-origin") ||
             noticeName(f[C.coep], "embedder policy value") ||
             noticeName(f[C.coepReportOnly], "report-only embedder policy value") ||
             noticeWord(f[C.referrerPolicy], _REFERRER_POLICIES, "referrer policy") ||
             tokenOf("policy") || tokenOf("policyReportOnly");
    case "navigable.swap": case "navigable.navigate":
      return parts(P.parts, false) || noticeName(f[P.document], "document") || noticeName(f[P.address], "address") ||
             noticeArrival(f[P.provenance], f[P.driven], f[P.pinned]) || noticeName(f[P.issuer], "issuer");
    case ROUTE_SEED_NOTICE:
      /* `observed` is unreachable for a declaration: HTML §7.4.4 is reached only by running the page's code. */
      return parts(6, true) || noticeUrl(f[1], "address") ||
             noticeWord(f[2], [PROVENANCE_DERIVED, PROVENANCE_FORCED], "provenance") ||
             noticeWord(f[3], [DRIVEN_YES, DRIVEN_NO], "drive mark") ||
             noticeWord(f[4], [PINNED_YES, PINNED_NO], "witness mark") || noticeName(f[5], "declaring document");
    case "windowproxy.post":
      return parts(6, false) || noticeName(f[1], "target document") || noticeWorld(f[2], "world") ||
             noticeMarks(f[3], "marks");
    case "remoteop.answer":
      /* The completion is the remainder and may contain a tab; this zone relays it unread. */
      return parts(5, false) || noticeName(f[1], "rendezvous token") || noticeName(f[2], "timeline") ||
             noticeName(f[3], "marks");
    case "fetch.cancel":
      return (f.length === 2 ? /^[1-9][0-9]*$/.test(f[1])
                             : f.length === 6 && f[1] !== "" && f[2] !== "" && f[4] !== "" && f[5] !== "")
        ? null : "names neither a rendezvous id nor the five-field pair";
    case "world.gone": case "world.parked":
      return parts(2, false) || noticeName(f[1], "world");
    case "remoteop.exhausted": case "remoteop.retracted":
      return parts(2, false) || noticeName(f[1], "rendezvous token");
    default:
      return null;
  }
}
/* Refuses the instance whose notice `line` breaks its record, unless the instance is already retired. */
function engineRefuseNotice(eng, line, why) {
  if (!eng._crashed) engineRefuse(eng, "the engine's notice `" + line.slice(0, 300) + "` " + why);
}
async function hostNotice(eng, line) {
  /* A retired instance is asked nothing more, and its owed notices are not acted on. */
  if (eng._crashed) return;
  const f = line.split("\t");
  const _shape = noticeRefusal(f);
  if (_shape !== null) { engineRefuseNotice(eng, line, _shape); return; }
  /* Field indices from lib/navigable-notice.js, which engine/noticegen.mjs writes from the engine's
     navigable_notice.h, so no index here is written by hand. */
  const C = NAVIGABLE_NOTICE.create, P = NAVIGABLE_NOTICE.provision;
  /* `navigable.request`, navigable_notice.h's request table — a child navigable's HTML §7.4.5 request facts,
     which a sub-frame seed of that address speaks with (seedChildRequest). */
  /* `navigable.open <child> <creator> <opener>`, navigable_notice.h's open table — an auxiliary navigable the
     engine created and holds. Nothing is provisioned: this zone records its opener, the creator or null, so a
     later navigation of it into a peer is bounded by this record rather than by the engine's word. The creator
     must be a document of this instance and the child's name minted from it, as for a create. */
  if (f[0] === "navigable.open") {
    const O = NAVIGABLE_NOTICE.open;
    DCHECK(f.length === O.parts && f[O.child] !== "" && f[O.creator] !== "" && f[O.opener] !== "" &&
           f[O.sandboxFlags] !== "" && f[O.provenance] !== "" &&
           (f[O.driven] === DRIVEN_YES || f[O.driven] === DRIVEN_NO) &&
           (f[O.pinned] === "pinned" || f[O.pinned] === "unpinned"),
           "a navigable.open notice does not match lib/navigable-notice.js's open table: " + line);
    const _opened = askerRecord(eng, [f[O.creator]], "creator");
    const _allowedOpener = { doc: f[O.creator], origin: eng.origin };
    const _openRefusal = _opened.refusal ||
      (docRecordNameParent(f[O.child]) !== f[O.creator] ? docRecordAskerRefusal("child", "not-minted", f[O.child])
                                                        : docRecordOpenerClaim(f[O.opener], _allowedOpener));
    if (_openRefusal) {
      console.warn("[bridge] navigable.open " + f[O.child] + " is refused: " + _openRefusal.text);
      engineCountRefusal(eng, _openRefusal.token);
      return;
    }
    /* The popup's record, composed by HTML §7.3.1.7's and §7.1.5's rules from the creator's record and never
       from the engine's answer to them (docRecordAuxiliary): null is the creator's sandboxed auxiliary
       navigation flag, which refuses the navigable and every navigation of it into a peer. */
    const _auxDoc = docRecordAuxiliary(_opened.doc, docRecord(f[O.provenance], f[O.driven], f[O.pinned],
                                                              f[O.sandboxFlags], docRecordAddressSpace(_opened.doc)));
    if (_auxDoc === null) {
      const _sandboxed = docRecordAskerRefusal("child", "sandboxed", f[O.child]);
      console.warn("[bridge] navigable.open " + f[O.child] + " is refused: " + _sandboxed.text);
      engineCountRefusal(eng, _sandboxed.token);
      eng.openers.set(f[O.child], { opener: null, refused: true });
      return;
    }
    eng.records.set(f[O.child], _auxDoc);
    eng.openers.set(f[O.child], { opener: docRecordNavigableIdentity(f[O.opener]) === null ? null : _allowedOpener,
                                  refused: false });
    return;
  }
  if (f[0] === "navigable.request") {
    const R = NAVIGABLE_NOTICE.request;
    childRequestRecord(f[R.creator], new URL(f[R.address]).href,
                       { destination: f[R.destination], referrerPolicy: f[R.referrerPolicy] });
    childNameRecord(f[R.creator], new URL(f[R.address]).href, f[R.child]);
    /* A same-origin child is a realm in this instance (navigable.c's child_in_this_agent), named nowhere in the
       pool, so this instance is the one that can still create its children. */
    if (originOf(f[R.address]) === eng.origin) childRequestsOf(f[R.child]).instance = eng;
    return;
  }
  if (f[0] === "navigable.create") {
    /* Already provisioned: every timeline whose history created the document announces it under one name
       (solver/world.c's world_mint_doc), and the instance that holds it serves them all — unless this create is
       at another address, which is two documents claiming one name. */
    if (hostHolderOf(f[C.child])) {
      if (_createdAddress.get(f[C.child]) !== f[C.address]) {
        const _readdressed = docRecordAskerRefusal("child", "readdressed", f[C.child]);
        console.warn("[bridge] navigable.create " + f[C.child] + " at " + f[C.address] + " is refused: " +
                     _readdressed.text + " (" + _createdAddress.get(f[C.child]) + ")");
        engineCountRefusal(eng, _readdressed.token);
      }
      return;
    }
    /* The child's record, before anything is opened or loaded for it: the creating document's record
       (askerRecord) composed with the notice's words by HTML's own rules (lib/document-record.js), so a child
       of a document this tool opened is never graded as one the person navigated to and inherits every mark
       and flag the rules carry. The creator is refused unless it is a document of this instance and the
       child's name is minted from it (docRecordNameParent, solver/world.c's world_mint_doc), the nesting the
       record of the child's own descendants walks. A navigable with no parent is an auxiliary one, which
       §7.3.1.7 does not create at all under the creator's sandboxed auxiliary navigation flag; that notice is
       refused here, whatever the engine decided. */
    const _created = askerRecord(eng, [f[C.creator]], "creator");
    const _createRefusal = _created.refusal || (docRecordNameParent(f[C.child]) !== f[C.creator]
      ? docRecordAskerRefusal("child", "not-minted", f[C.child]) : null);
    if (_createRefusal) {
      console.warn("[bridge] navigable.create " + f[C.child] + " is refused: " + _createRefusal.text);
      engineCountRefusal(eng, _createRefusal.token);
      return;
    }
    /* The opener is the creator or null (§7.3.1.7 step 8.3.8 against 8.3.7), whatever the engine claims. */
    const _openerRefusal = docRecordOpenerClaim(f[C.opener], f[C.parent] === "u"
      ? { doc: f[C.creator], origin: eng.origin } : null);
    if (_openerRefusal) {
      console.warn("[bridge] navigable.create " + f[C.child] + " is refused: " + _openerRefusal.text);
      engineCountRefusal(eng, _openerRefusal.token);
      return;
    }
    _createdAddress.set(f[C.child], f[C.address]);
    const _creatorDoc = _created.doc;
    const _childOwn = docRecord(f[C.provenance], f[C.driven], f[C.pinned], f[C.sandboxFlags],
                                docRecordAddressSpace(_creatorDoc));
    const _childDoc = f[C.parent] === "u" ? docRecordAuxiliary(_creatorDoc, _childOwn)
                                          : docRecordJoin(_creatorDoc, _childOwn);
    if (_childDoc === null) {
      console.warn("[bridge] navigable.create " + f[C.child] + " is refused: the creating document's sandbox sets " +
                   "HTML §7.1.5's sandboxed auxiliary navigation browsing context flag, so §7.3.1.7 creates no " +
                   "auxiliary navigable");
      return;
    }
    documentPolicyOpen(f[C.child]);
    /* The group: HTML §7.3.1.7's noopener arm creates the navigable given a null opener, in a new browsing context
       group (§7.3.2.1); a child and an auxiliary navigable with an opener are in the creator's group, where the
       peer holds a WindowProxy for the creator (its `parent`, or its `opener`) and the engine raised its own flag
       at the emission (core/frame/navigable.c). This is the zone's record of that, read when the instance is
       re-rooted and by the join arms below. */
    const _inCreatorGroup = f[C.group] === "creator";
    /* A child at an address already in the creator's agent (its group and origin) is one child_in_this_agent
       builds as a realm and announces to nobody, so the notice is the engine's statement and nothing is loaded. */
    if (_inCreatorGroup && principalOf(_childDoc, originOf(f[C.address])) === eng.origin) {
      documentPolicySettle(f[C.child], null);
      engineRefuseNotice(eng, line, "announces a child at an address in the creator's own agent cluster");
      return;
    }
    if (_inCreatorGroup) eng.referenced = 1;
    /* The provenance is the engine's word for what this navigation is evidence of, beside its drive mark and
       witness mark; navigationLoad and the chokepoint decide on them, and this arm passes them through. */
    /* `none`: a notice has no request id for a refusal, and its only arm for a bytes-less load would fabricate
       an empty document, so navigationLoad aborts on a decline instead, naming what to build. */
    const loaded = await eng.fetchedDocument(f[C.address], f[C.provenance], f[C.driven], f[C.pinned],
                                             /*refusalArm*/"none", DOCUMENT_REQUEST_UNSTATED,
                                             docRecordOpaque(_childDoc), _creatorDoc);
    childRequestAlias(f[C.creator], f[C.address], loaded.url);
    /* The child's principal is principalOf its record: a new opaque origin under the sandboxed origin flag
       (HTML §7.3.2.1 Creating browsing contexts), otherwise the origin of the response's URL (`loaded.url`,
       after redirects, which only this zone followed), never the notice's origin field: SECURITY.md makes
       routing and the origin stamped on a delivered message the trusted zone's alone. The same URL is the
       address the instance is rooted at (HTML §7.5.1 Shared document creation infrastructure's
       `creationURL`). */
    /* The child's browsing-context group is its creator's: a nested navigable is in its parent's group, and an
       auxiliary one too unless `noopener` severed it. So two cross-origin children of one page at one origin
       are one cluster, as HTML has them. */
    /* The child's document as bytes, passed unchanged (see engineCreate); a load that did not load carries
       none, and the empty byte sequence is the `about:blank`-shaped document the engine's child_document builds
       for one. */
    DCHECK(loaded && (loaded.bytes === null || loaded.bytes instanceof Uint8Array),
           "the document load answered neither bytes nor the null that means it did not load");
    DCHECK(loaded && typeof loaded.url === "string" && loaded.url !== "",
           "the document load answered no RESPONSE URL for a cross-origin child — the address the notice " +
           "carries is the one the creating engine ASKED for, and the principal this zone stamps on a peer " +
           "instance has to be the origin of the URL its bytes CAME from");
    const _childBytes = loaded.bytes === null ? new Uint8Array(0) : loaded.bytes;
    /* The record's reach, arrival and flag set are on every analyze record, with the address space of the
       document the load created: its response's, or the creator's for a local-scheme one (`_childDoc` is the
       navigable's initial document, whose container is the creator's clone). */
    const msg = { type: "AST_ANALYZE", pageHtml: _childBytes, sourceUrl: loaded.url,
                  origin: principalOf(_childDoc, originOf(loaded.url)),
                  groupId: _inCreatorGroup ? eng.groupId : "noopener:" + (_nextNoopenerGroup++),
                  provenance: _childDoc.reach, arrival: docRecordArrival(_childDoc),
                  sandbox: docRecordSandbox(_childDoc),
                  addressSpace: self.safeFetchDocumentAddressSpace(loaded.url, _creatorDoc),
                  responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    /* The response's own header list, whole; the creator's container is relayed separately below. */
    DCHECK(loaded && loaded.headers && typeof loaded.headers === "object",
           "the document load answered no header list — §7.5.1 creates the child's Document from its " +
           "response's headers, and a missing list is a producer that stopped writing one rather than a " +
           "response that carried none, which is the empty object");
    for (const _n of Object.keys(loaded.headers)) msg.responseHeaders[_n] = loaded.headers[_n];
    /* The load's Set-Cookie fields go to the jars their URLs' origins key in this document's group. */
    await zoneRouteSetCookies(msg.groupId, loaded.setCookies);
    /* The creator's clone is relayed as a container, never as a response header: a response header's policy
       gets the response URL's origin as its self-origin (CSP §2.2.2 Parse response's Content Security
       Policies), while CSP §2.2 Policies gives an inherited list the creator's self-origin, which §6.7.2.8 Does
       url match expression in origin with redirect count? reads. Mixing them answers `'self'` backwards. Both
       halves cross separately and the engine decides which container the Document gets (main.c's
       document_csp_list); this zone relays and computes only the child's principal.
       The two policy texts are BASE64 fields because a raw CSP header may contain HTAB, so no field holds a
       tab and the record splits exactly. */
    /* The embedder policy's four items are TOKEN fields: their values are fixed tokens, and `report-to` is a
       structured-field string (RFC 8941 §3.3.3 Strings: printable ASCII %x20-%x7E, no tabs). Relayed verbatim; only the receiving engine maps the tokens,
       and it crashes on an unknown one (see NEW_EMBEDDER_POLICY). */
    /* CSP §2.2's list crosses as its enforce and its report policies, BASE64 fields read through the table's
       reader. §7.1.7's referrer policy item is relayed verbatim beside the embedder policy: a Referrer Policy
       §3 token or "", none of which holds a tab. */
    const inherited = { csp: navigableNoticeField(C, f, "policy"),
                        cspReportOnly: navigableNoticeField(C, f, "policyReportOnly"),
                        selfOrigin: f[C.cspSelfOrigin],
                        embedder: { value: f[C.coep], endpoint: f[C.coepEndpoint],
                                    reportOnlyValue: f[C.coepReportOnly],
                                    reportOnlyEndpoint: f[C.coepReportOnlyEndpoint] },
                        referrerPolicy: f[C.referrerPolicy] };
    /* HTML §7.3.1.3 Child navigables' parent navigable, separate from the container policy: an
       auxiliary navigable (`window.open`) has a full container and no parent. It is the emitting engine's own
       navigable identity (core/frame/remote_object.h), relayed verbatim; only the receiving engine decodes it,
       and it crashes on an unparseable record. A one-letter tag and '.'-terminated base64 contain no tab. */
    const parentNavigable = f[C.parent];
    /* HTML §7.2.2.4's opener, in the parent's grammar (`N` for none); only the receiving engine
       decodes it (navigable_root_opener). */
    const openerNavigable = f[C.opener];
    /* The container answer, the other §7.3.1.3 link: a child `<iframe>` has both links and an
       auxiliary navigable has neither. It carries Permissions Policy §9.5's result, computable only in the
       creating engine, which holds the element and the child's origin. Relayed verbatim; feature tokens and
       `Enabled`/`Disabled` contain no tab. */
    const containerPolicy = f[C.containerPolicy];
    /* The internal ancestor origin objects list (HTML §3.1.3 Ancestor origins), a third algorithm
       over inputs neither field above carries. The answer travels rather than the inputs because §3.1.3 asks
       whether an ancestor is same origin with the parent document's origin, and opaque origins are compared by
       identity while all serialize to `null` (a `data:` document is in its own instance because it is
       opaque). Entries are origin serializations joined by SPACE, which URL §3.2 Host miscellaneous forbids in
       a host, as it does TAB. */
    const ancestorOrigins = f[C.ancestorOrigins];
    /* The creation sandboxing flag set (HTML §7.1.5 Sandboxing), not an item of the policy
       container; it reads the embedder element and its node document's active flags, so the creating engine
       computes it. Members are joined by COMMA because §7.1.5's flag names contain spaces. Relayed verbatim;
       the receiving engine crashes on a word outside that section. */
    const creationSandboxFlags = docRecordSandbox(_childDoc);
    /* The set is this zone's (the child's record), never empty: `none` is the empty set, and an empty one would
       provision a cross-origin `<iframe sandbox>` as unsandboxed. */
    DCHECK(typeof creationSandboxFlags === "string" && creationSandboxFlags !== "",
           "a child's record states no HTML §7.1.5 creation sandboxing flag set — docRecordSandbox serializes " +
           "every record's set, `none` for the empty one");
    /* The top-level creation URL (HTML §8.1.3.1 Environments) is the creator's decision (its own for a nested
       navigable, the navigable's address for an auxiliary one), carried because the new instance cannot see
       what embeds it; HTML §8.1.3.5 Secure contexts reads it. */
    /* The cluster decides which of two operations this is. A same-origin child never reaches here
       (navigable.c's `child_in_this_agent` builds it as a realm in the creator's heap and emits no notice), so a
       cluster hit is a second cross-origin document of one cluster (two `<iframe src="https://cdn/…">` of one
       page), which HTML puts in one heap: it is joined to the running instance. A cluster with no instance is
       provisioned one. */
    const _ckey = clusterKeyOf(msg);
    /* The address the engine asked for is outside the creator's agent (refused above, before the load), so a
       child in the creator's own cluster is a response that redirected into the creator's origin, which only
       this zone saw. */
    if (_ckey === eng.cluster) {
      DFAIL("a cross-origin child's response redirected into its creator's origin (" + f[C.address] + " -> " +
            loaded.url + "): the engine built it as a remote navigable before the response, and this zone has no " +
            "arm that turns it into a realm of the creator's heap. Build that commit of the response into the " +
            "creator's agent, as child_in_this_agent would have done for the landed address");
      documentPolicySettle(f[C.child], null);
      return;
    }
    /* Reservations answer too: engineCreate takes the pool slot before its first await, so a cluster being
       provisioned answers here as a provisioned one does. A reservation still booting is waited for (`qjs_join`
       needs `qjs_init` and `qjs_begin`), and asked again: one whose boot failed has left the pool, so the child
       is provisioned an instance of its own. */
    let holder = hostClusterOf(_ckey);
    if (holder && holder.state === "booting") { await holder._readyP; holder = hostClusterOf(_ckey); }
    if (holder) {
      DCHECK(holder.state === "hot" || holder.state === "fetching",
             "a cluster's instance answered in state `" + holder.state + "` after its boot settled — a settled " +
             "reservation is hot, or has left the pool");
      /* The joined document is referenced (the creating engine minted this name because its page holds a
         WindowProxy under it), so its instance may not be one whose timelines may run out. Every instance of a
         group other than its top-level one was provisioned for a navigable a peer created, and the top-level
         one is referenced as soon as it creates a navigable another instance hosts (the record at the top of
         this arm), so a holder reached through nesting that returns to its origin (A embeds cross-origin B, B
         embeds A) is A, the creator of B. */
      DCHECK(holder.referenced === 1,
             "a cross-origin child was joined to an instance that holds no peer reference — the create notice is " +
             "itself the proof that one exists (the creating engine minted this name because its page holds a " +
             "WindowProxy under it), and every instance of a group either was provisioned for a peer's " +
             "navigable or created one, so this holder came from neither and the first read through the new " +
             "child's proxy will arrive at a heap entitled to have finished");
      /* The bytes are on the message here, the one caller for which that holds: `msg` is the object this zone
         composed for the announced child. */
      await engineJoin(holder, msg.pageHtml, msg, f[C.child], f[C.topLevelUrl], inherited, parentNavigable,
                       openerNavigable, containerPolicy,
                       ancestorOrigins, creationSandboxFlags);
      return;
    }
    /* A child document joins the one pool and is ranked, sliced, parked and finalized like every other. It has
       no caller, so its findings merge like a rehydrated engine's (`cold`, stated at the reservation). Awaited,
       because one round's notices are acted on in order: a page opens a window and posts to it in one turn, so
       the instance must exist before the post is routed. Referenced, since the notice itself proves a
       WindowProxy is held for it. */
    await engineCreate("", msg.pageHtml, msg, false, f[C.child], f[C.topLevelUrl], inherited, parentNavigable,
                       openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags, 1)._readyP;
    return;
  }
  /* `navigable.swap <new document> <url> <origin> <provenance>` — HTML §7.1.3.2 Browsing context group
     switches due to opener policy: a navigation whose response's opener policy does not match builds its
     Document in a new top-level browsing context in a new browsing context group, and the old one is left
     behind (the emitting instance has recorded that, so the opener's handle answers `closed === true`).
     It must not join: an instance is (group, origin) and a swap changes the group, so even at an origin this
     pool runs it is a separate heap. The group id is minted here by a counter, a routing fact that is this
     zone's alone. The new context has a null creator, so no container or opener is inherited, and its
     top-level creation URL is its own address. This zone loads the bytes: an untrusted engine that supplied
     bytes and a principal could name any origin's document into existence.
     Named residual: the address is fetched again instead of committing the response the navigation already
     had. Next diff remembers this zone's own reply to that `document.fetch`, keyed by (instance, address).
     Absence shows as a swapped-to document whose content or headers differ from the response that decided
     the swap when the server answers the two requests differently. */
  if (f[0] === "navigable.swap") {
    /* The name is minted fresh for each swap (solver/world.h), so one this pool holds would provision one heap
       for two Documents. */
    if (hostHolderOf(f[P.document])) { engineRefuseNotice(eng, line, "names a document this pool already holds"); return; }
    /* The re-fetch is made under the same decision as the navigation's own load, since the provenance on this
       record is the load job's (core/frame/browsing_context_group.c takes it from there); a swap cannot fetch
       an address this zone declined at `document.fetch`. */
    /* `none`, for the create arm's reason: a swap is a notice, and its `pageHtml` line turns a null body into
       an empty Uint8Array. */
    /* The issuing document's record and the navigable's (issuedRecords) joined with the notice's words; a
       notice naming a document that is not this instance's is refused, as a request is. */
    const _swapFrom = issuedRecords(eng, f[P.issuer], f[P.document]);
    if (_swapFrom.refusal) {
      console.warn("[bridge] navigable.swap " + f[P.document] + " is refused: " + _swapFrom.refusal.text);
      return;
    }
    const _swapDoc = childDocRecord(_swapFrom.from, f[P.provenance], f[P.driven], f[P.pinned]);
    const swapped = await eng.fetchedDocument(f[P.address], f[P.provenance], f[P.driven], f[P.pinned],
                                              /*refusalArm*/"none", DOCUMENT_REQUEST_UNSTATED,
                                              docRecordOpaque(_swapDoc), _swapFrom.issuerDoc);
    DCHECK(swapped && (swapped.bytes === null || swapped.bytes instanceof Uint8Array),
           "the swapped-to document load answered neither bytes nor the null that means it did not load");
    DCHECK(swapped.headers && typeof swapped.headers === "object",
           "the swapped-to document load answered no header list — §7.5.1 creates its Document from the " +
           "response's headers, and §7.1.3 obtains the opener policy that decides its new group's cross-origin " +
           "isolation mode out of the same list");
    /* The response's URL, for the create arm's reason: the instance's principal and `creationURL` are the
       address the bytes came from. */
    DCHECK(typeof swapped.url === "string" && swapped.url !== "",
           "the swapped-to document load answered no RESPONSE URL — §7.1.3.2 provisions a new instance from " +
           "the address the Document is at, which after a redirect is not the address the swap requested");
    const swapMsg = { type: "AST_ANALYZE", pageHtml: swapped.bytes === null ? new Uint8Array(0) : swapped.bytes,
                      sourceUrl: swapped.url, origin: principalOf(_swapDoc, originOf(swapped.url)),
                      groupId: "swap:" + (_nextSwapGroup++),
                      provenance: _swapDoc.reach, arrival: docRecordArrival(_swapDoc),
                      sandbox: docRecordSandbox(_swapDoc),
                      addressSpace: self.safeFetchDocumentAddressSpace(swapped.url, _swapFrom.issuerDoc),
                      responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    for (const _n of Object.keys(swapped.headers)) swapMsg.responseHeaders[_n] = swapped.headers[_n];
    /* The load's Set-Cookie fields go to the jars their URLs' origins key in this document's group. */
    await zoneRouteSetCookies(swapMsg.groupId, swapped.setCookies);
    /* No inherited policy: a swapped-to Document has no creator to clone a container from (HTML §7.3.2.1 with
       a null creator), so a response without CSP is a Document under no policy. */
    DCHECK(hostClusterOf(clusterKeyOf(swapMsg)) === null,
           "§7.1.3.2's swap minted a browsing-context group this pool already runs an instance for — the group " +
           "id is a fresh counter, so a hit means two swaps were given one id and the second would JOIN the " +
           "heap the first built, which is exactly the boundary a group switch exists to draw");
    /* `null` container: the swapped-to context is created with a null creator, so HTML §7.1.7 has nothing to
       clone and the Document is judged against its own response. */
    /* The parent is `u`: HTML §7.1.3.2's swap is reached only for a top-level browsing context (the engine
       asserts it where the record is written), so the navigable has no parent. The ancestor list (HTML §3.1.3)
       is `none` for the same reason: a top-level context has no container document. */
    /* The top-level creation URL is `swapped.url`: the swapped-to navigable is a top-level traversable, so it is
       its own Document's address after redirects. */
    /* Not referenced: the old browsing context "will not be used by the new Document" (HTML §7.1.3.2's note)
       and the emitting engine discards it after writing this notice, so the opener's handle answers about the
       document it had (its `closed` is true) and nothing holds a proxy for this one. */
    await engineCreate("", swapMsg.pageHtml, swapMsg, false, f[P.document], swapped.url, null, "u", "N", "null",
                       "none", swapMsg.sandbox, 0)._readyP;
    return;
  }
  /* `navigable.navigate <new document> <url> <origin> <provenance>` — a TOP-LEVEL traversable navigated to a
     response whose origin is another agent's, with HTML §7.1.3.2 keeping the browsing context: the emitting
     engine's navigable becomes a remote one over this document (core/frame/navigable.c's nav_navigate_to_peer).
     The browsing context group is kept too, so the document joins the instance of its (group, origin) cluster
     when one runs and is provisioned one otherwise, as a cross-origin child is. It is a top-level document, so
     it has no creator container (the empty CSP pair and HTML §7.1.4's new embedder policy), no parent, no
     container and no ancestor list, and its own address is its top-level creation URL. Referenced, since the
     emitting engine's proxy names it. This zone loads the bytes, for the swap arm's reason. */
  if (f[0] === "navigable.navigate") {
    /* The name is minted fresh by the navigating engine, as a swap's is. */
    if (hostHolderOf(f[P.document])) { engineRefuseNotice(eng, line, "names a document this pool already holds"); return; }
    /* The records, as for a swap. */
    const _navFrom = issuedRecords(eng, f[P.issuer], f[P.document]);
    if (_navFrom.refusal) {
      console.warn("[bridge] navigable.navigate " + f[P.document] + " is refused: " + _navFrom.refusal.text);
      return;
    }
    /* HTML §7.2.2.4's opener survives the navigation, and the peer gets it only as this zone recorded it at the
       navigable's creation: the engine's claim may name that document or none. */
    const _allowed = docRecordOpenerOf(f[P.document], engineOpenerLookup(eng));
    const _navOpenerRefusal = _allowed.refusal || docRecordOpenerClaim(f[P.opener], _allowed.opener);
    if (_navOpenerRefusal) {
      console.warn("[bridge] navigable.navigate " + f[P.document] + " is refused: " + _navOpenerRefusal.text);
      engineCountRefusal(eng, _navOpenerRefusal.token);
      return;
    }
    const _navOpener = docRecordNavigableIdentity(f[P.opener]) === null ? "N" : f[P.opener];
    /* The peer holds a WindowProxy for its opener, a document of this instance, as for a create's opener. */
    if (_navOpener !== "N") eng.referenced = 1;
    const _navDoc = childDocRecord(_navFrom.from, f[P.provenance], f[P.driven], f[P.pinned]);
    /* A navigation to an address in the navigating instance's own agent is one it performs itself, so a notice
       sending it to a peer is the engine's statement and nothing is loaded. */
    if (principalOf(_navDoc, originOf(f[P.address])) === eng.origin) {
      engineRefuseNotice(eng, line, "navigates to a peer at an address in its own agent cluster");
      return;
    }
    const navd = await eng.fetchedDocument(f[P.address], f[P.provenance], f[P.driven], f[P.pinned],
                                           /*refusalArm*/"none", DOCUMENT_REQUEST_UNSTATED,
                                           docRecordOpaque(_navDoc), _navFrom.issuerDoc);
    DCHECK(navd && (navd.bytes === null || navd.bytes instanceof Uint8Array) &&
           navd.headers && typeof navd.headers === "object" && typeof navd.url === "string" && navd.url !== "",
           "the navigated-to document load answered without bytes-or-null, a header list and a response URL — " +
           "the instance is provisioned from all three");
    const navMsg = { type: "AST_ANALYZE", pageHtml: navd.bytes === null ? new Uint8Array(0) : navd.bytes,
                     sourceUrl: navd.url, origin: principalOf(_navDoc, originOf(navd.url)), groupId: eng.groupId,
                     provenance: _navDoc.reach, arrival: docRecordArrival(_navDoc),
                     sandbox: docRecordSandbox(_navDoc),
                     addressSpace: self.safeFetchDocumentAddressSpace(navd.url, _navFrom.issuerDoc),
                     responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    for (const _n of Object.keys(navd.headers)) navMsg.responseHeaders[_n] = navd.headers[_n];
    /* The load's Set-Cookie fields go to the jars their URLs' origins key in this document's group. */
    await zoneRouteSetCookies(navMsg.groupId, navd.setCookies);
    const _nkey = clusterKeyOf(navMsg);
    /* The address the engine stated is outside its own agent (refused above, before the load), so a document in
       its own cluster is a re-fetch that landed there: the response changed between this zone's two loads. */
    if (_nkey === eng.cluster) {
      DFAIL("a top-level navigation into a peer re-fetched " + f[P.address] + " and landed in the navigating " +
            "instance's own origin (" + navd.url + "): this zone loads the document twice and has no arm for the " +
            "two loads disagreeing. Build the commit of the navigation's own response, which the swap arm's named " +
            "residual also needs");
      return;
    }
    let navHolder = hostClusterOf(_nkey);
    if (navHolder && navHolder.state === "booting") { await navHolder._readyP; navHolder = hostClusterOf(_nkey); }
    if (navHolder) {
      DCHECK(navHolder.referenced === 1,
             "a top-level navigation into a peer joined an instance that holds no peer reference — the " +
             "navigating engine's proxy names this document, so that instance may not let its timelines run " +
             "out, and every instance of a group either was provisioned for a peer's navigable or created one");
      /* No creator: the join's grammar for that is the empty CSP pair and the new embedder policy, as for a
         tab's top-level navigation joining its cluster. */
      await engineJoin(navHolder, navMsg.pageHtml, navMsg, f[P.document], navd.url,
                       { csp: "", cspReportOnly: "", selfOrigin: "", embedder: NEW_EMBEDDER_POLICY,
                         referrerPolicy: DEFAULT_REFERRER_POLICY }, "u", _navOpener, "null", "none",
                       navMsg.sandbox);
      return;
    }
    /* No creator: engineCreate's grammar for that is `null`, as for the swap arm. */
    await engineCreate("", navMsg.pageHtml, navMsg, false, f[P.document], navd.url, null, "u", _navOpener, "null",
                       "none", navMsg.sandbox, 1)._readyP;
    return;
  }
  /* `document.seed <address> <provenance> <driven> <pinned> <issuer>` — an address the application declared is a page of itself, reached
     when forced execution ran the bundle's own `history.pushState`/`replaceState` (solver/route_seed.h says what
     a declaration is). This branch spends no network: a declared route is a work item whose fetch is a cost the
     Level-1 order carries, so it records and `admit` loads (see `_seeds`). */
  if (f[0] === ROUTE_SEED_NOTICE) {
    /* A route of this document, established from the two addresses: HTML §7.2.5 The History interface's
       can-have-its-URL-rewritten already refuses a cross-origin rewrite in the engine, and this zone checks the
       same thing from its own facts. The comparison decides no principal (the load reads under `eng.origin`).
       It is a refusal, not an assert, because the engine is untrusted. */
    if (originOf(f[1]) !== originOf(eng.msg.sourceUrl)) {
      console.warn("[bridge] a route declaration named an address outside the declaring document's origin (" +
                   f[1] + " vs " + eng.msg.sourceUrl + ") — HTML §7.2.5's can-have-its-URL-rewritten permits " +
                   "no such rewrite, so this is not a page of that application and it is not seeded");
      return;
    }
    /* An address the chokepoint would not fire is not enqueued. `safeFetchFiringRefusal` is the same function
       the chokepoint refuses with (`_firingRefusal`), asked here because enqueuing a load it will refuse would
       spend an admission slot every round. It answers the refusing grade, which the warning names. For a
       navigation the address is the whole of the safety, and a navigation carries the person's session. A
       derived but unfired address is still reported. */
    /* `document` and the record's witness mark, as navigationLoad states them; a document load is never
       script-like. */
    /* The facts arrive as one object so the growing signal set cannot shift positional operands. `credentialed`
       is the answer this seed's load would get, asked with the same predicate; `headers` is absent on a
       navigation and stated as such. The principal is `eng.origin` (MessageSender.origin, the same value the
       seed carries as `principalOrigin`), never a URL-derived origin, which would answer `true` for a sandboxed
       document. */
    /* `doc` is the declaring document's record (issuedRecords), since this asks about a request issued by that
       document; a declaration naming a document that is not this instance's is refused, as a request is. */
    const _seedFrom = issuedRecords(eng, f[5], null);
    if (_seedFrom.refusal) {
      console.warn("[bridge] a route declaration for `" + f[1] + "` is refused: " + _seedFrom.refusal.text);
      return;
    }
    /* `actor: "tool"`: the load this asks about is this zone's act, as the load states (the seed arm says why). */
    const _seedRefusal = self.safeFetchFiringRefusal({
      url: f[1], destination: "document", provenance: f[2], pinned: f[4],
      doc: _seedFrom.issuerDoc, actor: "tool", driven: f[3],
      credentialed: navigationCarriesSession(f[1], eng.origin), headers: null });
    if (_seedRefusal) {
      console.warn("[bridge] a route declaration for `" + f[1] + "` is refused by this origin's egress " +
                   "policy on `" + _seedRefusal + "` — the address is derived and reported, and it is not " +
                   "seeded; the signal and value named there is the row of the person's own control that " +
                   "holds it");
      return;
    }
    /* The work item takes its inputs with it: the principals belong to the declaring document, not the declared
       address (its address bases the load and its record's address space classifies the SSRF host, so an
       address cannot authorize itself), and the pool may no longer hold that engine at admission. The
       provenance is stored from the record (always `derived` here, since a refused one returned above);
       navigationLoad asserts the vocabulary on the way out. */
    await level1AskSites([f[1]]);
    /* The declaring document's reach grade travels with the item for the same reason. */
    /* `doc` is the seed's own record, the declaring document's joined with the declaration's words, and the
       identity is the one its load would have: navigationLoad's own session predicate over that record. */
    const _seedDoc = childDocRecord(_seedFrom.from, f[2], f[3], f[4]);
    const _seedId = documentIdentityOf(f[1], eng.origin,
                                       navigationCredentialed(f[1], eng.origin, docRecordOpaque(_seedDoc)));
    _seedCensus.declared++;
    if (_seeds.has(_seedId)) _seedCensus.landedOnDeclared++;
    else _seeds.set(_seedId, { url: f[1], identity: _seedId, principalUrl: eng.msg.sourceUrl,
                               principalOrigin: eng.origin, provenance: f[2], driven: f[3], pinned: f[4],
                               fromDoc: _seedFrom.issuerDoc, doc: _seedDoc });
    /* This router runs inside serviceFetch, so the kick finds a round in flight and wakes it if it is waiting
       on another engine; `admit` ranks this entry next round. */
    _hostKick();
    return;
  }
  if (f[0] === "windowproxy.post") {
    const target = hostHolderOf(f[1]);
    /* As a cross-agent operation's target: never provisioned in this group is the engine's statement, and
       provisioned and ended is this zone's lifecycle. */
    if (target === null) {
      const _never = provisionedRefusal(f[1], eng.groupId);
      if (_never !== null) { engineRefuseNotice(eng, line, _never); return; }
      /* A crashed or finalizing target receives nothing, as a cross-agent operation's does. */
      const _ended = _provisioned.get(f[1]).ended;
      DCHECK(_ended !== null && _ended !== "finalized",
             "a message was posted to `" + f[1] + "`, which this zone provisioned in this group and records as `" +
             _ended + "` with no instance holding it — either the pool lost a live document or finalized one as " +
             "complete while a peer still held a WindowProxy for it");
      return;
    }
    /* A target mid-boot is still the target, and the delivery waits for it: the reservation carries the name
       from the start, so routing now would post into `null` and refusing would drop a delivery the page already
       holds a WindowProxy for. */
    /* A target whose boot failed has crashed and receives nothing, as a crashed provisioned target does. */
    if (target.state === "booting") await target._readyP;
    if (target.state !== "hot" && target.state !== "fetching") return;
    /* The stamp is the sender's browser-stated origin, serialized. An empty one is an instance rehydrated from a
       recipe written before the principal was recorded; this zone does not know whose message it is and must
       not invent one. A CHECK, because in release the delivery would carry a fabricated identity into a
       security decision. */
    CHECK(!!eng.origin, "a cross-document message was posted by an instance with no recorded principal — only " +
                        "the trusted zone may state a sender's origin, and this one has none to state; carry " +
                        "the document's browser-stated origin into the cold recipe so a resumed instance keeps it");
    /* The one firing rule, as a fetch is judged (safeFetchCrossDocumentRefusal), under the sending document's
       record: the head of the record's world names it (docRecordWorldDocument), and a name that is no document
       of this instance is refused, as a request's asker is. */
    const _sent = askerRecord(eng, [docRecordWorldDocument(f[2])], "asker");
    if (_sent.refusal) {
      console.warn("[bridge] windowproxy.post -> " + f[1] + " is refused: " + _sent.refusal.text);
      engineCountRefusal(eng, _sent.refusal.token);
      return;
    }
    const _postRefusal = self.safeFetchCrossDocumentRefusal({ targetOrigin: target.origin, marks: f[3],
                                                              doc: _sent.doc });
    if (_postRefusal) {
      console.warn("[bridge] windowproxy.post -> " + f[1] + " is refused on `" + _postRefusal + "`, as a fetch " +
                   "with the same signals would be");
      engineCountRefusal(eng, "blocked-signal:" + _postRefusal);
      return;
    }
    await target.r.renderer.route({ record: line, senderOrigin: stampOrigin(eng.origin) });
    return;
  }
  /* `remoteop.answer <token> <world> <marks> <completion>` — a completion this instance produced for an
     operation it was asked to perform, under the token this zone minted. It is a notice, not a return value, because a peer
     answers by running a program (an IDL getter, a page's setter or function) as a flow on its own frontier.
     Relayed verbatim and unread: the completion is in remote_object.c's grammar, where an object crosses as a
     name in the answering agent's namespace, which only an engine can interpret. */
  if (f[0] === "remoteop.answer") {
    const to = _remoteOps.get(f[1]);
    /* The token is echoed verbatim by the instance that performed the operation, so one this zone never minted
       names no asker. */
    if (to === undefined) { engineRefuseNotice(eng, line, "answers a rendezvous token this zone never minted"); return; }
    /* The world is the third field (world_serialize's grammar, separators ':' and ','), the marks the fourth,
       the completion the remainder (it may contain a tab). This zone reads none of them but must know where
       each ends; noticeRefusal has checked the first two are present. */
    const _t2 = line.indexOf("\t", line.indexOf("\t") + 1);
    const _t3 = line.indexOf("\t", _t2 + 1);
    const _t4 = line.indexOf("\t", _t3 + 1);
    /* Every completion is relayed: a peer's document state is its flows, so one question has one true answer per
       timeline, and the asking flow forks an arm over each. Keeping one per token would answer a page from
       whichever timeline the interleaving left. The engine refuses a repeat of one timeline at its delivery
       entry. */
    await to.asker.r.renderer.hostAnswerRemote({ request: to.req, world: line.slice(_t2 + 1, _t3),
                                                 marks: line.slice(_t3 + 1, _t4),
                                                 completion: line.slice(_t4 + 1) });
    return;
  }
  /* `world.gone <world>` — a world of this instance is finished (its flow left the frontier, or the session
     parked into a generation that will never mint again). Every peer that received a delivery or operation from
     it holds a COW segment keyed on it (solver/world.h) until told, and cannot park while it does (cold_park
     refuses a foreign segment). Broadcast by design: the sender does not track which peers a flow reached,
     since releasing an unheld world is a no-op, and only this zone knows the other instances. A booting
     instance is skipped because both delivery branches wait for `_readyP`, so it holds no foreign world
     (qjs_world_gone asserts the frontier was seeded). */
  /* `fetch.cancel <identity>` — no flow is owed this request any more; the identity is a park's five-field pair
     or a rendezvous id (inflightKey). */
  if (f[0] === "fetch.cancel") {
    engineCancel(eng, f.slice(1));
    return;
  }
  if (f[0] === "world.gone") {
    for (const peer of _pool) {
      if (peer === eng) continue;
      if (peer.state !== "hot" && peer.state !== "fetching") continue;
      await peer.r.renderer.worldGone({ world: f[1] });
    }
    return;
  }
  /* `remoteop.exhausted <token>` — no timeline of this instance holds the question any more and the last one let
     it go by answering or refusing, so no answer follows. The asker retires the standby its family keeps for
     later timelines' answers, and the token is spent. */
  if (f[0] === "remoteop.exhausted") {
    const done = _remoteOps.get(f[1]);
    if (done === undefined) {
      engineRefuseNotice(eng, line, "exhausts a rendezvous token this zone never minted or has already spent");
      return;
    }
    _remoteOps.delete(f[1]);
    done.asker._remoteAsked.delete(done.req);
    await done.asker.r.renderer.hostAnswersExhausted({ request: done.req });
    return;
  }
  /* `remoteop.retracted <token>` — an instance is parking with a question it was asked and hands it back,
     started or not: a half-run answering program's work is the parking flow's own delta, and what was asked
     for was a call (HTML §7.2.1.3.5 CrossOriginGet ( O, P, Receiver ) ends "Return ? Call(getter, Receiver)"),
     so an abandoned call was made zero times. One notice per question, sent by the last holding timeline.
     This zone forgets: `_remoteAsked` suppresses re-asks because `engine_host_requests` does not dedupe, and
     the asking flow is still suspended and re-reported every step, so lifting the suppression is the re-ask.
     Nothing is stored: a token is this session's in-memory name and never enters a residue; across a restart
     the asker's own recipe re-issues the request under a new id. */
  if (f[0] === "remoteop.retracted") {
    const back = _remoteOps.get(f[1]);
    /* An unknown token is a name the engine minted itself, so no asking flow is released by it. */
    if (back === undefined) { engineRefuseNotice(eng, line, "retracts a rendezvous token this zone never minted"); return; }
    back.asker._remoteAsked.delete(back.req);
    _remoteOps.delete(f[1]);
    return;
  }
  /* `world.parked <world vector>` — a park wrote the foreign segments this instance holds into its residue
     (solver/cold.h's 'w' record), so a peer's timeline outlives the session that held it. `world.gone` is
     broadcast only to live instances, so a death announced while this document is parked is missed and the
     resuming instance would hold a segment for an ended world for the rest of its process.
     What to build: a persisted index from world name to the parked document carrying a segment for it (the
     frontier entry is keyed by address|identity|bundle, so this is a reverse map or a store of its own), surviving a
     browser restart as the residue does, drained into the resuming instance's `worldGone` after `begin`. */
  if (f[0] === "world.parked") {
    DFAIL("an engine parked while carrying a PEER's world segment across the cold tier (`" + f[1] + "`), and " +
          "this zone holds no death queue for a cold document. Every `world.gone` announced while this " +
          "document is parked is broadcast to the LIVE pool alone and lost, so the instance that resumes it " +
          "rebuilds a segment for a world that has ended and never releases it. Build the index this notice " +
          "exists to make exact — world name -> the parked document carrying a segment for it, persisted as " +
          "far as the residue is — and drain it into the resuming instance's `worldGone` after `begin`");
  }
  DFAIL("an engine emitted a notice with an op this zone does not act on — the engine's half is built, so the " +
        "host's half is the unbuilt one: `" + f[0] + "`");
}

// What only this zone can answer. A cross-document operation is answered by the instance holding that document,
// and only the offscreen knows which instance that is (SECURITY.md). The asking flow is suspended mid-frame
// until the answer lands, so this is pumped every round beside the fetch replies; an unanswered one parks that
// flow while its siblings keep running.
/* It returns how many registers it filled, for engineDeliverLanded's reason; a branch that answers nothing (an
   op this zone may not guess at) counts none, leaving the asking flow parked and visible. */
async function engineServiceHostRequests(eng) {
  let did = 0;
  // One-way notices: this zone owes each an action, handled in order and one at a time, because a page opens a
  // window and posts to it in the same turn and the create must finish provisioning before the post is routed.
  for (const line of owedList("GetHostNotices", (await eng.r.renderer.getHostNotices()).notices)) {
    await hostNotice(eng, line);
    did++;
  }
  if (eng._crashed) return did;
  // Only ops this zone can answer are answered: `document.fetch` is a network fetch through the safeFetch
  // chokepoint, and a cross-agent operation is asked of the peer. Every other op stays unanswered: the asking
  // flow stays parked with its snapshot intact and qjs_host_requests keeps reporting it, which is visible where
  // a guessed answer is not. Answering is what lets a navigation finish.
  const reqs = owedList("GetHostRequests", (await eng.r.renderer.getHostRequests()).requests);
  for (const line of reqs) {
    /* A retirement can land at any await of this loop, and a retired instance is asked nothing more. */
    if (eng._crashed) return did;
    const req = hostRequestOf(line);
    if (typeof req === "string") { engineRefuse(eng, req); return did; }
    const { id, asker, op } = req;
    /* The asking document's record, which the two arms this zone performs judge by. A request whose asker is no
       document of this instance is declined whatever its operation, by the rule the pending line's askers are
       held to; a declined rendezvous is not listed again. */
    const _asked = askerRecord(eng, [asker], "asker");
    if (_asked.refusal) {
      console.warn("[bridge] host request " + id + " is refused: " + _asked.refusal.text);
      await engineDeclineRendezvous(eng, id, _asked.refusal.token);
      did++;
      continue;
    }
    const askerDoc = _asked.doc;
    /* A cross-agent operation is not answered by this zone: it is asked of the peer holding the document.
       `otherW.length` (HTML §7.2.2.2 Indexed access on the Window object) counts the peer's child navigables,
       and the internal methods a lent object performs ([[Get]], [[Set]], [[Delete]], [[Call]]) run the peer's
       code, so an answer computed here would be wrong. This zone carries the record to the holder (SECURITY.md:
       only it knows which instance that is) and the completion back.
       Matched by prefix, not a list, so a row added to core/frame/remote_op.c's OPS table under either
       namespace needs nothing here: each names its target document in the same field. `windowproxy.close` is
       one of them, the definitely-close task of a traversable whose document the peer holds. Nothing is
       answered inside the ask; the peer answers by running a program as a flow, and the completion arrives
       through its notices (hostNotice). */
    if (op.startsWith("windowproxy.") || op.startsWith("object.")) {
      /* An unanswered request is re-reported every step (engine_host_requests does not dedupe), so asking on
         every sighting would perform the peer's operation once per step. */
      if (eng._remoteAsked.has(id)) continue;
      const _opf = op.split("\t");
      const _opShape = noticeName(_opf[1], "target document") || noticeMarks(_opf[3], "marks");
      if (_opShape !== null) { engineRefuse(eng, "the engine's host request `" + op.slice(0, 300) + "` " + _opShape); return did; }
      const holder = hostHolderOf(_opf[1]);
      /* No holder has two readings, told apart by this zone's record: a name never provisioned in this group is
         the engine's invention, and one provisioned and since ended while a peer still referenced it is this
         zone's lifecycle. */
      if (!holder) {
        const _never = provisionedRefusal(_opf[1], eng.groupId);
        if (_never !== null) { engineRefuse(eng, "the engine's host request `" + op.slice(0, 300) + "` " + _never); return did; }
        /* A peer that crashed, or is being finalized, answers nothing, and the asking flow stays parked, listed
           as an unanswered request. One that finished normally while referenced is this zone's bug: an instance
           whose document is referenced may not finish (`referenced`). */
        const _ended = _provisioned.get(_opf[1]).ended;
        DCHECK(_ended !== null && _ended !== "finalized",
               "a cross-agent operation named `" + _opf[1] + "`, which this zone provisioned in this group and " +
               "records as `" + _ended + "` with no instance holding it — either the pool lost a live document " +
               "or finalized one as complete while a peer still held a WindowProxy or a lent object of it");
        continue;
      }
      /* This zone routes an operation only to the instance holding its target, so a target in the asking
         instance is the engine's statement: an operation on its own agent's object is performed in its heap. */
      if (holder === eng) {
        engineRefuse(eng, "the engine's host request `" + op.slice(0, 300) + "` routes a cross-agent operation to " +
                          "a document of its own instance");
        return did;
      }
      /* The one firing rule, as a fetch is judged (safeFetchCrossDocumentRefusal), under the asking document's
         record; the record's fourth field is the asking timeline's marks (engine_record_head). A refusal is a
         declined rendezvous, which the asker forks into a waiting arm and a failure arm. */
      const _opRefusal = self.safeFetchCrossDocumentRefusal({ targetOrigin: holder.origin, marks: _opf[3],
                                                              doc: askerDoc });
      if (_opRefusal) {
        await engineDeclineRendezvous(eng, id, "blocked-signal:" + _opRefusal);
        did++;
        continue;
      }
      const token = "op" + (_nextRemoteToken++);
      _remoteOps.set(token, { asker: eng, req: id });
      eng._remoteAsked.add(id);
      /* The ask is recorded before the suspending call, so the next round, re-listing the same id, does not
         perform the operation again. */
      await holder.r.renderer.perform({ token, record: op });
      did++;
      continue;
    }
    // The XMLHttpRequest fetch (XHR §3.5.6 The send() method) is the other answer this zone can give: a network
    // fetch through safeFetch. The record carries method, headers and body because the chokepoint decides SOP,
    // CORS, method and credentials. A synchronous XMLHttpRequest's flow stays suspended at its `send()` line; an
    // asynchronous one's runs on, and the answer is delivered to it as a task.
    if (op.startsWith("xhr.send\t")) {
      /* Issued, not awaited, through the same door as the pending seam, since `fetchedXhr` reaches the same
         `_readBody`. Keyed on the request id, not a (method, url) pair (wpt_runner.c's `wpt_request_asked_id`
         beside `wpt_request_asked`): the answer is delivered against `engine_host_answer`'s id, and two
         identical questions from two flows are two ids. */
      did += engineIssue(eng, inflightKey([String(id)]),
        (signal) => eng.fetchedXhr(op.slice("xhr.send\t".length), askerDoc, signal),
        // Completion 0 is normal (ECMA-262 §6.2.4 The Completion Record Specification Type): this zone fetched
        // bytes and ran no program that could throw. A relayed cross-agent operation answers 1 with the thrown
        // value, so the asking page's `try`/`catch` runs.
        (r) => r.declined ? engineDeclineRendezvous(eng, id, r.declined) : engineAnswer(eng, id, r.meta, r.bytes));
      continue;
    }
    if (!op.startsWith("document.fetch\t")) continue;
    /* `document.fetch<TAB><provenance><TAB><driven><TAB><pinned><TAB><destination><TAB><referrer policy><TAB>
       <initiator><TAB><url>`, split at the first seven tabs after the verb so the address is the remainder. A URL
       cannot contain a tab (URL Standard §4.4 URL parsing strips them), the first four tokens are lowercase words,
       the referrer policy is a Referrer Policy §3 policy or "" (core/frame/navigable.c's HTML §7.4.5 request after
       Fetch §4.1 Main fetch), and the initiator is HTML §7.4.2.2's sourceDocument by its engine name. Indexed
       rather than split on every tab, which would silently take the tail of a malformed record. */
    const df = documentFetchOf(op.slice("document.fetch\t".length));
    if (typeof df === "string") { engineRefuse(eng, df); return did; }
    const fetchRequest = { destination: df.destination, referrerPolicy: df.referrerPolicy };
    /* Issued, not awaited, for the XHR seam's reason (a page states a document load with `location.href` or an
       `iframe src`, and its body may never end), keyed on the request id. The asking navigable stays parked on
       its load; every other flow keeps running. */
    did += engineIssue(eng, inflightKey([String(id)]),
      /* `rendezvous`: engineDeliverDocument routes `r.declined` to `hostDecline` against the id the engine is
         parked on, so the navigable keeps the `about:blank` HTML §7.3.1.3 created it holding. */
      () => eng.fetchedDocument(df.url, df.provenance, df.driven, df.pinned, /*refusalArm*/"rendezvous",
                                fetchRequest, docRecordOpaque(askerDoc), askerDoc),
      /* A same-origin child's load files its landed address under its create's facts, as a cross-origin one's
         does in the `navigable.create` arm. */
      (r) => {
        childRequestAlias(df.initiator, df.url, r.url);
        return engineDeliverDocument(eng, id, r);
      });
    continue;
  }
  return did;
}
/* One owed host request, `id<TAB>asker<TAB>op`, written by engine_host_requests from a counter that starts at 1
   and is CHECKed against wrapping; the id is the whole routing table for an answer (engine_host_answer matches
   every flow's register on it). The line is the instance's statement: answers {id, asker, op}, or the reason it
   is refused, which retires the instance rather than leaving its flow parked with no symptom. */
function hostRequestOf(line) {
  const tab = line.indexOf("\t");
  const atab = tab > 0 ? line.indexOf("\t", tab + 1) : -1;
  if (!(tab > 0 && atab > tab + 1))
    return "content.mojom.Renderer.GetHostRequests answered `" + line + "`, which is not `id<TAB>asker<TAB>op`";
  const id = +line.slice(0, tab);
  if (!Number.isInteger(id) || id <= 0)
    return "content.mojom.Renderer.GetHostRequests answered `" + line + "`, whose id is not a positive integer";
  return { id, asker: line.slice(tab + 1, atab), op: line.slice(atab + 1) };
}
/* A document.fetch request's fields after the verb: `<provenance><TAB><driven><TAB><pinned><TAB><destination>
   <TAB><referrer policy><TAB><initiator><TAB><url>`, which core/frame/navigable.c writes on every path, the policy
   possibly empty. It is split at the first six tabs so the address is the remainder: a URL cannot contain a tab
   (URL Standard §4.4 URL parsing strips them), the first three tokens are solver/engine.h's words, the
   destination is a Fetch §2.2.5 navigation destination, the policy a Referrer Policy §3 policy or "" (HTML §7.4.5's
   request after Fetch §4.1 Main fetch), and the initiator HTML §7.4.2.2's sourceDocument by its engine name.
   Answers the fields, or the reason the record is refused. */
function documentFetchOf(args) {
  const f = [];
  let at = 0;
  for (let k = 0; k < 6; k++) {
    const t = args.indexOf("\t", at);
    if (t < 0) return "a document.fetch request carries fewer than seven fields: `" + args + "`";
    f.push(args.slice(at, t));
    at = t + 1;
  }
  f.push(args.slice(at));
  const [provenance, driven, pinned, destination, referrerPolicy, initiator, url] = f;
  const why = (provenance !== PROVENANCE_OBSERVED && provenance !== PROVENANCE_DERIVED && provenance !== PROVENANCE_FORCED)
      ? "provenance `" + provenance + "`"
    : (driven !== DRIVEN_YES && driven !== DRIVEN_NO) ? "drive mark `" + driven + "`"
    : (pinned !== PINNED_YES && pinned !== PINNED_NO) ? "witness mark `" + pinned + "`"
    : NAVIGATION_DESTINATIONS.indexOf(destination) < 0 ? "destination `" + destination + "`"
    : _REFERRER_POLICIES.indexOf(referrerPolicy) < 0 ? "referrer policy `" + referrerPolicy + "`"
    : initiator === "" ? "empty initiator"
    : url === "" ? "empty address" : null;
  if (why !== null) return "a document.fetch request carries the " + why + " this zone does not speak: `" + args + "`";
  return { provenance, driven, pinned, destination, referrerPolicy, initiator, url };
}
/* The document load's delivery, named so the answer is handed over against the id it was fetched under. */
async function engineDeliverDocument(eng, id, r) {
  // JSON, so the answer carries its type (a null body is a load that did not load, "null" is a document); the
  // body crosses as bytes beside it (HostAnswer's `array<uint8>?`), since a Document is parsed from bytes.
  /* The answer of HTML §7.4.5 Populating a session history entry: the response's URL, its header list as HTTP
     field lines (the form qjs_init takes, so a navigated and a rooted Document are built from one shape), and
     the document as bytes. The URL is the response's (Fetch §2.2.6 Responses), not the requested address:
     §7.4.5 determines the Document's origin, and so its agent cluster, over it, and only this zone saw the
     redirect chain. */
  DCHECK(typeof r.url === "string" && r.url !== "",
         "the document load answered no RESPONSE URL — fetchedDocument states one on every arm, including " +
         "the ones where the load did not load, because §7.4.5 determines a Document's origin over it and a " +
         "navigable whose load failed still gets a Document");
  /* A refusal goes to the other entry. `HostAnswer` settles the rendezvous and NAV_LOAD_CREATE runs over the
     value, right for a failed load (an error page is a real navigation outcome) and false for a load nobody
     made. `HostDecline` records the refusal on the same rendezvous, and the engine's `flow_decline_fork` forks:
     one arm keeps waiting with no value (the navigable keeps the `about:blank` HTML §7.3.1.3 Child navigables
     created it holding, and fires once the origin is widened), the other takes the network error and becomes
     the error-page document with its path marked forced. Both worlds are explored. */
  if (r.declined) return engineDeclineRendezvous(eng, id, r.declined);
  DCHECK(Array.isArray(r.setCookies) && r.setCookies.every((c) => Array.isArray(c) && c.length === 2 &&
                                                              typeof c[0] === "string" && typeof c[1] === "string"),
         "a document load answered no [URL, Set-Cookie field] list — navigationLoad states one on every arm");
  const _ownCookies = await zoneAnswerSetCookies(eng, r.setCookies);
  await engineAnswer(eng, id, { url: r.url, headers: responseFieldLines(r.headers), setCookies: _ownCookies },
                     r.bytes);
}
/* A refusal of a synchronous request (a document load or an XHR fetch), recorded on the rendezvous it names.
   This zone issued the request against an id the engine had just published, so an answer matching no parked
   flow is the engine's statement that `GetHostRequests` and this answer have drifted, and refuses it. */
async function engineDeclineRendezvous(eng, id, reason) {
  const matched = await eng.r.renderer.hostDecline({ request: id, reason });
  if ((!matched || matched.matched !== 1) && !eng._crashed)
    engineRefuse(eng, "the engine answered a decline of host request " + id + " as matching " +
                      JSON.stringify(matched && matched.matched) + " parked flow(s), not one");
}
/* The two channels for a synchronous answer. The document load and the XHR fetch carry a fetched body as bytes;
   every other answer (a number, a document name) has `bytes === null`. Completion 0 is ECMA-262 §6.2.4's
   normal completion: this zone fetched bytes and ran no program that could throw. */
async function engineAnswer(eng, id, meta, bytes) {
  await eng.r.renderer.hostAnswer({ request: id, answer: JSON.stringify(meta), completion: 0, body: bytes });
}
async function engineFinalize(eng) {
  /* Ask the engine for its result (qjs_result, the same composition qjs_emit_partial makes), before teardown,
     since the document is built from the context teardown frees. Without it a page that finished before the
     first partial cadence would have no document. A crashed instance is not asked (its memory is what aborted);
     the lines it already printed are in this zone's buffer and read below. */
  if (!eng._crashed) {
    let json = null;
    /* An engine abort is a recorded outcome; a host invariant failure is not. An ABI call rejects for both (the
       frame's abort, or this zone's contract failure as apiclientFatal), and reporting the second as the
       engine's crash would blame the instance and discard the page's findings. */
    try { json = (await eng.r.renderer.getResult()).result; }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "result", e); }
    /* No document is pushed for an empty answer, and linesToAnalysis refuses a complete run without one. */
    if (!eng._crashed) {
      if (typeof json === "string" && json !== "") eng.lines.push("@RESULT " + json);
    }
  }
  /* Outstanding rendezvous go with the asking instance: a completion relayed into a torn-down instance would be
     a call into a removed frame. The engine's engine_host_answer already treats a request whose flow is gone as
     unanswered by anyone. */
  for (const [token, to] of _remoteOps) if (to.asker === eng) _remoteOps.delete(token);
  /* Outstanding requests go the same way: the flows parked on them went with the instance, and their addresses
     are in the residue just written, so a resumed recipe re-issues them against current sources. The promises
     still settle into records nothing reads. */
  eng._inflight.clear();
  /* A crashed instance is not torn down: `qjs_teardown` walks the engine's gc_obj_list to report leaks, a
     finding about a runtime that ran, and a dead renderer cannot serve another call (renderer-host's assert
     would report a second crash). The frame is removed regardless; that is the whole cleanup, since the
     module, its linear memory and every retained qjs_init argument die with it, and an iframe left in this
     document is never collected. */
  if (!eng._crashed) {
    try { await eng.r.renderer.teardown(); }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "teardown", e); }
  }
  engineFrameDestroy(eng);
  /* A crashed run's findings stay and the run is labelled: streamPartial has already merged every snapshot into
     the cumulative store, and hiding them from the analysis would show the document that learned them as
     clean. The completeness claims a crash invalidates are refused where each is made: no cost counters in the
     run log, no frontier write (finish), and a per-document crash marker the popup renders. */
  const result = linesToAnalysis(eng.lines, eng.msg, eng._crashed ? "crashed" : "complete", eng);
  result._fkey = eng.fkey; result._prior = eng.prior;   // engine-computed key + parked entry -> persisted below
  return result;
}
/* A WASM abort is the engine crashing. It stays scoped to this engine, so one page cannot kill the scheduler
   serving other tabs, but it is impossible to overlook: a console.error banner, a persistent crash count, a
   `_run:"crashed"` record every consumer reads, and no completeness claim anywhere (no counters, no frontier
   write). What the engine already observed is not discarded (see engineFinalize). */
function crashBanner(stage, m) {   // every abort path (create, step, teardown) routes here, so no crash is quiet
  /* Neither operation can throw (an increment of a declared number, a console write), so nothing here is
     caught: the crash path must always announce itself. The counter is declared at load, so "no crash"
     differs from "bridge.js not loaded". */
  self._engineCrashOccurred++;
  console.error("\n==== ENGINE CRASH (" + stage + ") — " +
                (stage === "result-refused" ? "THIS ZONE REFUSED THE INSTANCE'S ANSWER" : "WASM ABORTED") +
                ", run marked crashed, NOT swallowed ====\n" + m + "\n");
}
// The C-side CHECK/DCHECK prints its @WHY/@E root line (phase/cond/at/reason) to the frame's line buffer
// immediately before abort(), so every crash path puts that root line in the banner and the crash record:
// emscripten's "native code called abort()" alone names no cause.
/* One scan for both callers, which differ only in where the lines come from: a live engine's buffer, or the
   lines renderer-host attaches to the rejection of the call that aborted. */
function rootWhyLine(lines) {
  for (let i = lines.length - 1; i >= 0; i--) {
    const ln = String(lines[i]);
    if (ln.startsWith("@WHY ") || (ln.startsWith("@E ") && /"(reason|cond|phase)"/.test(ln))) return ln;
  }
  return "";
}
function engineCrash(eng, stage, e) {
  const m = String((e && e.message) || e);
  eng._crashed = true;
  const root = rootWhyLine(eng.lines);
  const err = root ? (m + " | ROOT: " + root) : m;   // the crash record carries its cause, not only the console banner
  eng.lines.push('@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(err) + "}");
  crashBanner(stage, err);
}
/* This zone refusing an instance's answer (a result document, a Level-1 reply): the instance is retired as
   crashed with the refusal as its cause, and linesToAnalysis reads the `result-refused` stage into the run's
   `result-refused` row. Its memory did not abort, so no root @WHY is appended; it is asked nothing more
   (`step` and engineFinalize skip a crashed instance) and is finalized at its next step. */
function engineRefuse(eng, why) {
  DCHECK(typeof why === "string" && why !== "", "an instance was refused with no stated reason");
  DCHECK(!eng._crashed, "an instance was refused after it was retired — every refuser reads the crashed mark " +
                        "after its last await, so a retired instance's answer is never read");
  eng._crashed = true;
  eng.lines.push('@E {"phase":"engine-crash","stage":"result-refused","err":' + JSON.stringify(why) + "}");
  crashBanner("result-refused", why);
}
// A crash before the engine ran a line of the page (a creation or boot abort): `_run:"crashed"`, never a quiet
// empty result.
/* The banner is not part of this function: engineBootFailed banners once per instance and calls this once per
   waiting caller (a RESHIP re-delivery, a sub-frame, a document that joined while it booted), each answering a
   different caller, so N callers do not count N crashes. */
/* `eng` is the reservation that failed, passed because a boot can die after `begin`: it may already have been
   handed a residue and told how many flows came back. A reservation that died earlier carries its declared
   `null`. */
function crashRecord(stage, m, msg, eng) {
  /* No result document is expected: the instance aborted before it could answer. The record goes through
     linesToAnalysis's own no-document arm, so both crash producers share one shape. It owns no run row: each
     waiting caller is a different document, and the instance-level fact (one abort) is the crash count. */
  return linesToAnalysis(['@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(m) + "}"], msg, "crashed", eng);
}

/* Macrotask yield. Between engine quanta the host returns to the event loop with a macrotask (an awaited
   microtask never interleaves with the message queue), so the one thread services its message port while a
   lone engine keeps exploring across qjs_step re-entries. MessageChannel is sub-millisecond, while HTML §8.7
   Timers clamps a nested zero-delay timer ("If nestingLevel is greater than 5, and timeout is less than 4, then
   set timeout to 4"), a third of the quantum. This is a thread yield, not a cap.
   There is no `setTimeout` fallback: it would silently make every slice three times late. It is a CHECK, not a
   DCHECK, because without a macrotask source a lone engine would never service its port in the build that
   ships. */
const PARTIAL_MS = 750;   // incremental-merge cadence: a hot engine surfaces its current findings this often
CHECK(typeof MessageChannel !== "undefined",
      "this realm has no MessageChannel, so the host has no sub-millisecond macrotask to return to its event loop on " +
      "between two engine quanta — a lone engine would hold the one thread across qjs_step re-entries and never service " +
      "its message port. There is deliberately no setTimeout fallback: HTML §8.7 'Timers' clamps a nested zero-delay " +
      "timer to at least 4ms, a third of the 12ms quantum, and a slice silently three times late is the plausible datum " +
      "this CHECK exists instead of. Build the macrotask source this realm does have, and name it here.");
const _macroChan = new MessageChannel();
function macroYield() {
  return new Promise((res) => { _macroChan.port1.onmessage = () => res(); _macroChan.port2.postMessage(0); });
}
/* ─── The Level-1 census ─────────────────────────────────────────────────────────────────────────────────
   The order the host took, written where it was taken: it is composed from `engineKey` per hot instance and
   `level1Key` per waiting address and cold row, and no engine can see another, so no result document can
   carry it. The unit is a round, not a pick: a frozen rank or a deleted weight shows only in the spread and
   in what is absent; one round asks both the resident scan and `_bestCandidate`, whose comparison is the
   Level-1 question; and nothing suspends inside the scan, so a round's weights are consistent.
   Presence, never a zero, separates the states: `self._level1 === undefined` (file not loaded), `null` (no
   round yet), `cands` absent (the round never asked the non-resident order; `booting` and `loading` say
   what held admission), `cands: 0` (asked, ranked nothing; the `excl*` rows say what was taken out), and
   `cands: n` with rows (a reading). Counts are present whenever their walk ran, weights only over a
   non-empty population of served members. Each row is read at its own instant (candidates when asked, the
   resident half at the rank, pool counts at round end), so `candDocs: 1` beside `waiting: 0` is the round
   having seated that document. No infinity enters (the record crosses `chrome.runtime.sendMessage` as JSON):
   nothing runnable is `drained`, never served is `unserved`/`unservedSites`, and `wTop`/`wMin` are site weights. */
let _level1Round = 0;
/* The distribution over rounds. `_level1` holds the last round, a gauge at an instant this loop chose, and a
   round that steps an engine and takes the yield arm records `hot: 0` by construction. So round shapes are
   counted over the lifetime and ride every record: the gauge says where the loop is, these say what it has
   been doing. They are an exact partition: a round assigns `rd.shape` at its single exit and `_level1Record`
   is the only incrementer; a round that leaves by a throw has no shape and is counted as `rThrew`. */
const _level1Shapes = { rNoPool: 0, rIdle: 0, rWaited: 0, rReleased: 0, rFinished: 0, rServiced: 0, rAdmitted: 0,
                        /* A round that left by a throw (`rThrew`) and one whose exit named a shape with no arm
                           (`rUnknown`) are two arms. The second is reachable only in release, where the DCHECK
                           naming a new exit is compiled out; without it the increment would key a missing name
                           and store NaN. */
                        rUnknown: 0, rThrew: 0 };
const _LEVEL1_SHAPE_KEY = { nopool: "rNoPool", idle: "rIdle", waited: "rWaited",
                            released: "rReleased", finished: "rFinished", serviced: "rServiced",
                            admitted: "rAdmitted" };
/* The one arm that is two populations, split by the engine's own step code. `rServiced` counts every round
   that ended in a service round, reached for ENGINE_STEP_STALLED (nothing runnable, so leaving the hot set is
   right) and for ENGINE_STEP_YIELD (a runnable frontier at the work quantum, whose round returns at once and
   which hostSchedule waits for while it outranks every hot engine). The two readings take opposite work, so
   they are counted apart; `testing/corpus`'s report.mjs prints them per pass in its ENGINE SPAN block. A
   yielding engine is not stepped while its payment is in flight: that would issue `ops.step` concurrently with
   its own `serviceFetch` on one renderer port, with `eng._inflight` mutated from two async chains.
   The seat-kind partition that explains a pool of many engines for one document is `rendererPoolProbe`'s
   `reservations` lifetime counters; the corpus census (`testing/corpus/site.mjs`) does not yet record them.
   This is its own object, not two more `_level1Shapes` arms, because those sum to `round` and these sum to one
   of them. It counts the code and never branches on it (see the paragraph at `rd.shape = "serviced"`). */
const _level1Serviced = { rServicedYield: 0, rServicedStalled: 0,
                          /* A code with no arm is an arm, for `rUnknown`'s reason: without it a widened step
                             enumeration would be counted as a yield, or store NaN, in the build that cannot
                             assert. */
                          rServicedUnknown: 0 };
const _LEVEL1_STEP_KEY = { 2: "rServicedYield", 3: "rServicedStalled" };
function _level1Record(pool, rd) {
  /* The shape is counted before the row is composed, so `round` and the arms are one instant. An empty shape is
     a round that left by a throw (every normal exit assigns one), counted as `rThrew`. */
  DCHECK(rd.shape === "" || _LEVEL1_SHAPE_KEY[rd.shape] !== undefined,
         "the Level-1 round recorded shape `" + rd.shape + "`, which this partition has no arm for — the arms " +
         "sum to `round` by construction, so an unknown shape is a new exit added to the loop without a row, " +
         "and the dev build refuses it here rather than letting release carry it as `rUnknown` unread");
  _level1Shapes[rd.shape === "" ? "rThrew" : (_LEVEL1_SHAPE_KEY[rd.shape] || "rUnknown")]++;
  /* The service partition is counted at the same instant as the arm it refines; shape and step code are
     assigned on the same two lines, so they are a biconditional. */
  DCHECK((rd.shape === "serviced") === (rd.stepCode !== null),
         "the Level-1 round recorded a service round with no step code, or a step code with no service round — " +
         "the engine's own {YIELD, STALLED} answer is assigned on the same two lines as the shape, so a " +
         "disagreement is a third exit having learned to service without saying which bench it took, and the " +
         "partition below would silently stop summing to the arm it refines");
  if (rd.stepCode !== null) {
    DCHECK(_LEVEL1_STEP_KEY[rd.stepCode] !== undefined,
           "the Level-1 round serviced an engine that answered step code `" + rd.stepCode + "` — the yield arm " +
           "is reached for ENGINE_STEP_YIELD (2) and ENGINE_STEP_STALLED (3) and for nothing else (the round " +
           "enumerates the codes and DONE takes the terminal arm), so a third value here is that enumeration " +
           "having been widened without this partition gaining the arm, which release would carry as a NaN");
    _level1Serviced[_LEVEL1_STEP_KEY[rd.stepCode] || "rServicedUnknown"]++;
  }
  const r = { round: ++_level1Round, pool: pool.length, booting: _bootingCount(),
              /* Seats whose document is still on the network: a second reason admission was not asked, so a
                 second row (folded into `booting` it would claim a reservation that does not exist). */
              loading: _loadingCount(),
              waiting: _waiting.length, hot: rd.hot === null ? 0 : rd.hot.n,
              /* Which arm could have asked the order: under the floor the comparison is admission's, at it
                 eviction's. */
              atFloor: _atRamFloor() ? 1 : 0 };
  if (rd.hot !== null && rd.hot.n > 0) {
    r.drained = rd.hot.drained;
    r.unserved = rd.hot.unserved;
    r.unservedSites = rd.hot.unservedSites;
    if (rd.hot.n - rd.hot.drained - rd.hot.unserved > 0) { r.wTop = rd.hot.wTop; r.wMin = rd.hot.wMin; }
  }
  /* The non-resident half arrives whole or not at all (`_bestCandidate` composes it in one walk). `candAsk`
     rides with it because a round can ask the order twice (an admission that reaches the RAM floor is followed
     by an eviction asking over the changed pool), and the record says so. */
  if (rd.cand !== null) {
    for (const k of Object.keys(rd.cand)) r[k] = rd.cand[k];
    r.candAsk = rd.candAsk;
  }
  /* The producer asserts its own grammar here, the opposite split from `_wfq`: this zone composes this record,
     so its names are stated once above, and popup.js renders whatever rows arrive and asserts only the shape. */
  for (const k of Object.keys(r))
    DCHECK(typeof r[k] === "number" && Number.isFinite(r[k]),
           "the Level-1 census composed a non-finite `" + k + "` — every row of it is a population count, a " +
           "0/1 state or a weight over a non-empty population, and the one value that is legitimately not a " +
           "number (an engine's -Infinity for a drained frontier) is reported as the `drained` COUNT rather " +
           "than folded into an extremum, so a non-finite here is a term that lost its presence rule");
  /* `hot` is the round's scan taken before the step and is not bounded by `pool`: a terminal round finalizes
     the engine (finish splices it out) before this runs in the round's `finally`, so `hot: 1` beside `pool: 0`
     is a completed document. `booting` and `pool` are read together, and `hot` is a filter of `pool` where it
     is computed, so nothing between `hot` and `pool` is asserted. */
  /* `booting` and `loading` are disjoint pool members (one state each), so their sum, read on the same line as
     `pool`, is asserted not to outrun it; a seat counted in both would make `loading` look like the reason. */
  /* The lifetime distribution rides every record unconditionally: it is what every earlier round did. */
  for (const k of Object.keys(_level1Shapes)) r[k] = _level1Shapes[k];
  for (const k of Object.keys(_level1Serviced)) r[k] = _level1Serviced[k];
  /* The arms sum to `round`, asserted: this function is the only incrementer and every exit assigns a shape, so
     a disagreement is a new exit with none, which would otherwise arrive as a silent `rThrew`. */
  DCHECK(Object.keys(_level1Shapes).reduce((n, k) => n + _level1Shapes[k], 0) === r.round,
         "the Level-1 round shapes sum to " +
         Object.keys(_level1Shapes).reduce((n, k) => n + _level1Shapes[k], 0) + " against " + r.round +
         " round(s) — this function is the ONLY incrementer and every exit of the round body assigns a shape, " +
         "so a sum that disagrees is an exit that assigns none being counted as a throw, or a second caller");
  /* And the service partition sums to `rServiced`: one assignment site, one incrementer, one instant. */
  DCHECK(r.rServicedYield + r.rServicedStalled + r.rServicedUnknown === r.rServiced,
         "the Level-1 census splits " + r.rServiced + " service round(s) into " + r.rServicedYield +
         " yield, " + r.rServicedStalled + " stalled and " + r.rServicedUnknown + " unknown — these are one " +
         "arm's own partition, counted from the " +
         "same `finally` as the arm, so a sum that disagrees is a round that serviced an engine without " +
         "relaying the engine's statement of which bench it was given");
  DCHECK(r.booting + r.loading <= r.pool,
         "the Level-1 census reports " + r.booting + " booting record(s) and " + r.loading + " loading seat(s) " +
         "in a pool of " + r.pool + " — all three are read at the same instant, at the end of the round, and " +
         "both populations are DISJOINT members of the pool they are counted against, so a sum that outruns it " +
         "is either a reading taken across a mutation or one seat counted as two reasons admission was held");
  DCHECK(("wTop" in r) === ("wMin" in r) &&
         ("wTop" in r) === (r.drained !== undefined && r.hot - r.drained - r.unserved > 0),
         "the Level-1 census reports a resident order whose extrema and whose served, runnable population " +
         "disagree — the weights exist exactly when a hot engine has a finite weight");
  DCHECK(!("wTop" in r) || r.wMin <= r.wTop,
         "the Level-1 census reports a resident order whose bottom outranks its top — these are the extrema " +
         "of one scan over one array of weights, so an inversion is two scans over two sets");
  /* The non-resident half is asserted whole against `_candCensus()`'s own row set, the one statement of which
     rows a walk that ran produces, so an exclusion row added to the walk cannot be dropped silently. */
  for (const k of Object.keys(_candCensus()))
    DCHECK(("cands" in r) === (k in r),
           "the Level-1 census carries part of the non-resident order — `" + k + "` and the total are one " +
           "walk's output and arrive together, so a census holding one half would report an order with no " +
           "exclusions as one that excluded nothing, or a population as an order that was never asked");
  DCHECK(("cands" in r) === ("candAsk" in r),
         "the Level-1 census holds a non-resident order without saying which of the round's two arms asked " +
         "it (or says so with no order to qualify) — under the floor the comparison against `wMin` is " +
         "admission's and at it it is eviction's, so a reading with no ask is two instants presented as one");
  DCHECK(!("candAsk" in r) || (r.candAsk === 1 || r.candAsk === 2),
         "the Level-1 census says the non-resident order was asked " + r.candAsk + " time(s) in one round — " +
         "there are exactly two arms that ask it (admission under the floor, eviction at it) and each asks " +
         "at most once, so anything else is an ask this record did not see and whose reading it is not holding");
  /* The populations account for the total, summed over CAND_KINDS so a new kind cannot be left out; a kind
     counted into the total by anything but `_candRanked` has no population and fails here. */
  DCHECK(!("cands" in r) || r.cands === CAND_KINDS.reduce((n, k) => n + r[k.pop], 0),
         "the Level-1 census ranked " + r.cands + " candidate(s) and its populations account for " +
         CAND_KINDS.map((k) => k.pop).join(" + ") + " of them — every work item with no instance is one of " +
         "the declared kinds, so a total that outruns them is an item counted into the order and accounted " +
         "for by no population, which the admission would then build through whichever arm is the fallback");
  DCHECK(CAND_SPREAD.every((s) => (s.row in r) === ("cands" in r && r.cands > r.candUnserved)) &&
         CAND_KINDS.every((k) => !(k.wMax in r) || r[k.pop] > 0),
         "the Level-1 census reports a spread over no served candidate, or omits one over a population that " +
         "has served members — the absence of a reading is how this record says nothing finite was ranked");
  DCHECK(!("candWMax" in r) || (r.candWMin <= r.candWMax && r.candRateMin <= r.candRateMax && r.candSvcMax > 0),
         "the Level-1 census reports a candidate order whose lowest-ranked item outranks its highest — one " +
         "walk produces both, so an inversion is the spread being taken over a different set from the pick, " +
         "and the spread is what can show a rank frozen at a constant");
  DCHECK(!("cands" in r) || (r.candSiteUnserved <= r.candUnserved && r.candUnserved <= r.cands),
         "the Level-1 census reports " + r.candSiteUnserved + " candidate(s) in a never-served site and " +
         r.candUnserved + " never-served candidate(s) of " + r.cands + " — a document of a never-served site is " +
         "never served itself, and one walk counts all three");
  DCHECK(r.unservedSites === undefined || r.unservedSites <= r.unserved,
         "the Level-1 census reports more hot documents in a never-served site (" + r.unservedSites + ") than " +
         "never-served hot documents (" + r.unserved + ")");
  self._level1 = r;
}

/* The scheduler policy, with engine ops injected so it is testable with mock engines. Each iteration admits a
   waiting document while there is headroom (`ops.admit` gates creation), then advances the highest-weight hot
   engine by one slice, which ends at the work quantum, and re-ranks. At the RAM floor `ops.evictee` chooses an
   engine to park to the cold tier (`ops.requestPark`). */
async function hostSchedule(pool, ops) {
  DCHECK(typeof ops.arrival === "function",
         "the pool was driven with no arrival op — a round waiting on an in-flight engine is woken by a new " +
         "work item only through it, so without it a never-served site waits on another document's reply");
  for (;;) {
    /* The round's reading, collected by the round and written once in `finally`, so every exit records: the
       `break`s, the `continue` that waits, and a round that dies inside an op (which records the half it reached;
       the other half is absent, read against `_hostDead`). */
    /* `stepCode` is null for every round that did not service; it is assigned on the same lines as
       `shape = "serviced"`, and `_level1Record` asserts the biconditional. */
    const rd = { hot: null, cand: null, candAsk: 0, shape: "", stepCode: null };
    /* Taken before admission asks, so an item that arrives after the ask wakes this round's wait arm. */
    const arrived = ops.arrival();
    try {
    if (ops.admit) rd.cand = await ops.admit();   // seat waiting documents into freed slots
    /* Admission answers with the order it took, or `null` where it took none; `undefined` would be an arm that
       stopped answering, asserted here where the caller is known. */
    DCHECK(rd.cand === null || (rd.cand && typeof rd.cand === "object" && Number.isInteger(rd.cand.cands)),
           "the admission op answered the round with something that is not a candidate-order reading — it " +
           "returns the census `_bestCandidate` composed, or null where it never asked the order, and an " +
           "undefined answer is an arm that returns nothing being read as an order that was never taken");
    if (rd.cand) rd.candAsk++;
    /* A never-served site is ahead of every served one, so admission seated its document, and the round's
       advance is spent on that admission: every new site is seated, and handed its first quantum, before a site
       deepens. A never-served document of a served site is ahead only within its site, so it is seated as room
       allows and waits for its site's share. */
    if (rd.cand && rd.cand.candSiteUnserved > 0) { rd.shape = "admitted"; continue; }
    if (!pool.length) { rd.shape = "nopool"; break; }
    const hot = pool.filter((e) => e.state === "hot");
    if (!hot.length) {   // every live engine is mid-something: wait for the earliest to become hot, then re-rank
      rd.hot = { n: 0, drained: 0 };   // a rankable set of none is a reading; the census omits the weights, not the row
      /* Three states reach this arm, each not rankable yet with a promise saying when: an engine awaiting a reply
         body, a reservation being provisioned, and an admission whose document is still on the network
         (`loading`). Without the second the loop would spin through admit while a boot was outstanding; the third
         is why a remote body that never ends no longer freezes this loop (the fetch is a pool member, so the hot
         set keeps being ranked and stepped). The wait is unbounded and resolves whether the bytes arrive or the
         load refuses; the next iteration re-asks. */
      const pending = pool.filter((e) => e.state === "fetching" || e.state === "booting" ||
                                         e.state === "loading");
      if (!pending.length) { rd.shape = "idle"; break; }
      for (const e of pending)
        DCHECK(e._readyP && typeof e._readyP.then === "function",
               "an engine in state `" + e.state + "` carries no readiness promise — this arm is the only " +
               "thing that resumes the pool when nothing is hot, so an engine it cannot wait on is one the " +
               "loop spins on or abandons, and both are silent");
      rd.shape = "waited";
      await Promise.race([arrived, ...pending.map((e) => e._readyP)]);
      continue;
    }
    /* The Level-1 pick, from one reading per engine (each account is what the last round with that instance
       recorded) over one service total. The ranking is synchronous: every account was recorded by an earlier
       round, so the scan has no suspension. The calls below suspend, so the pick is as of the top of this
       iteration, which is the policy (rank, advance, re-rank). */
    const tree = ops.tree();
    const ks = hot.map((e) => ops.key(e, tree));
    let bi = 0;
    for (let i = 1; i < hot.length; i++) if (level1Ahead(ks[i], ks[bi])) bi = i;
    const best = hot[bi], bestK = ks[bi];
    /* The reading of the resident order: nothing runnable is counted as `drained`, a never-served document as
       `unserved` and one of a never-served site as `unservedSites`; the extrema are over the site weights of
       the served, runnable documents. */
    let _fin = 0, _dr = 0, _un = 0, _unS = 0, _top = 0, _min = 0;
    for (const k of ks) {
      if (k.siteW === -Infinity) { _dr++; continue; }
      if (k.siteW === Infinity) _unS++;
      if (k.docW === Infinity) { _un++; continue; }
      if (_fin === 0) { _top = _min = k.siteW; }
      else { if (k.siteW > _top) _top = k.siteW; if (k.siteW < _min) _min = k.siteW; }
      _fin++;
    }
    rd.hot = { n: hot.length, drained: _dr, unserved: _un, unservedSites: _unS, wTop: _top, wMin: _min };
    /* An engine whose service round is in flight is not waited for, however it ranks: that round can wait on a
       server for as long as the server holds a reply, which charges the leader no service and so never lowers
       its weight. The pick is among hot engines, so a leader's return waits at most one quantum of a lesser one. */
    /* Normally step `best`. At the RAM floor, step the engine the one order says must give up its memory, after
       flagging it to park (evicted to the cold tier as replay recipes) so the item that outranks it gets the
       RAM; parking needs a step (the flag is read inside qjs_step), so that engine is targeted directly. A lone
       engine is evictable too: a wasm Memory never shrinks, so exempting it would let the first instance at the
       floor close admission for the session. `ops.evictee` asks whether anything not resident is worth more than
       the worst resident engine. */
    let target = best, evicting = false;
    DCHECK(typeof ops.evictee === "function" && typeof ops.requestPark === "function",
           "the pool was driven with no eviction op — the RAM floor is answered by the WFQ giving up the " +
           "lowest-value resident engine, so without it the floor is answered by refusing every admission " +
           "forever and one document holds the whole extension");
    DCHECK(typeof ops.release === "function",
           "the pool was driven with no release op — a Clear cannot take the frame of an engine this round " +
           "has a call outstanding on, so this round is what gives it back, and without it every Clear during " +
           "an analysis leaves a whole WASM instance resident under a document that does not reload");
    /* The order it asked, if any. Usually a round asks once, but `admit` can seat a document whose instance puts
       the working set at the floor, so eviction asks again over the changed pool; the record holds the later
       reading (the one `level1Ahead(cand.key, worst)` was decided by) and `candAsk` says it was asked twice. */
    const ev = await ops.evictee(hot);
    DCHECK(ev && typeof ev === "object" && "evict" in ev && "cand" in ev,
           "the eviction op answered the round with something other than the pair it decides — the engine " +
           "that must give up its RAM (or null) AND the reading of the non-resident order it decided that " +
           "against (or null where it never asked), so a bare answer is the round's one look at that order " +
           "at the floor going unrecorded, which is the instant it matters most");
    if (ev.cand) { rd.candAsk++; rd.cand = ev.cand; }
    if (ev.evict) { target = ev.evict; evicting = true; await ops.requestPark(ev.evict); }
    /* The rule the optimism terms exist for, asserted at the pick, one level at a time. A served site deepens
       only when the order asked this round held no document of a never-served site: admission seats one (the
       `admitted` arm above) and eviction parks a resident engine for one. A round that asked no order (no
       headroom below the floor) is the loading-seat residual `_admissionHasHeadroom` names; a never-served
       document of the picked site waiting on a seat is that same residual, one level down. */
    const _servedSite = Number.isFinite(bestK.siteW);
    DCHECK(evicting || !_servedSite || rd.cand === null || rd.cand.candSiteUnserved === 0,
           "Level 1 stepped a document of a served site (" + bestK.site + ") while the order it asked this round " +
           "held " + (rd.cand ? rd.cand.candSiteUnserved : 0) + " item(s) of a never-served site — a never-served " +
           "site is ahead of every served one, so admission or eviction should have taken this round");
    DCHECK(!_servedSite || _unS === 0,
           "Level 1 picked a served site (" + bestK.site + ") over " + _unS + " hot document(s) of a never-served " +
           "site — a never-served site weighs +Infinity among sites");
    /* Named residual. Not covered: within a site every never-served document is ahead of every served one, so a
       site that mints barren copies spends its whole share on them before its productive document deepens. Next
       diff builds: a level between site and document, documents grouped by their address template. Absence
       shows as: a site's served document taking no slice while `unserved` stays above zero for that site. */
    DCHECK(bestK.docW === Infinity ||
           ks.every((k) => k.site !== bestK.site || k.docW !== Infinity),
           "Level 1 picked a served document of " + bestK.site + " while a never-served hot document of that " +
           "site waited — a never-served document weighs +Infinity within its site");
    /* The park flag and the step that reads it stay ordered: both are calls on one renderer's point-to-point
       port, which delivers in order, and this loop makes no other call into that instance between them. */
    const st = await ops.step(target);
    /* The step op answers one of three codes: ENGINE_STEP_DONE (0), ENGINE_STEP_YIELD (2) or
       ENGINE_STEP_STALLED (3). A yield asks to be outranked and a stall asks to be paid, so they are distinct
       codes. The op refuses any other code the instance states, so a fourth here is the op's own contract. */
    DCHECK(st === 0 || st === 2 || st === 3,
           "the step op answered " + JSON.stringify(st) + ", outside {DONE, YIELD, STALLED} — `step` refuses an " +
           "instance's other codes through engineRefuse and answers DONE for them");
    // Dev `__forcepark`: request the park only after N dispatches, so it captures mid-exploration residue
    // (recipes with real decision vectors and handler-driven async flows), like a production RAM-pressure park.
    /* Only while there is a session to park: DONE means the step drained the frontier and closed the session,
       and the engine aborts on a park of nothing (storing an empty residue over a real one is the corruption the
       park prevents). A small fixture can drain within the two dispatches. */
    if (st !== 0 && target._forceparkSteps > 0 && --target._forceparkSteps === 0) await ops.requestPark(target);
    // Incremental merge: a lone unbounded engine may never reach DONE, so its emitted breadth is snapshotted and
    // merged on a coarse cadence on every non-final step.
    /* Awaited, for correctness: streamPartial asks the engine to print a result document, which arrives on that
       call's reply, then finds it by index in the line buffer and splices it out; the service round below
       appends to the same buffer, so an unawaited partial would splice at a moved index. */
    if (st !== 0 && ops.streamPartial) await ops.streamPartial(target);
    /* An engine that left the pool mid-round goes no further, and this round owns its frame. Only a Clear removes
       an engine while this loop holds it, and a Clear cannot destroy a renderer with a call outstanding, so it
       leaves the frame to the owner of that call. One check, placed after every suspending op, catches a drop
       during any of them (each op is safe on a dropped engine, and streamPartial refuses to merge one); without
       it `finish` would ask a dead instance for a result and write a residue into a frontier just emptied.
       The membership test uses this function's own `pool`, so the policy stays free of frames; `release` is the
       teardown. */
    if (pool.indexOf(target) < 0) { rd.shape = "released"; await ops.release(target); continue; }
    if (st === 0) {   // fully explored, or self-parked under RAM pressure: finalize (residue to the IndexedDB cold tier)
      rd.shape = "finished";
      await ops.finish(target);
    } else {   // ENGINE_STEP_YIELD (a cooperative quantum) or ENGINE_STEP_STALLED (a bill), see below
      /* Pay everything the engine says it is owed in one round: replies parked flows wait on, lazy chunks,
         notices this zone must act on, and synchronous requests only it can answer.
         Both codes take this arm by decision: engine.c states the schedule, paying at every slice boundary,
         because paying only at a stall makes one flow's reply wait for every other flow to block, worse as
         forks multiply. The codes differ in what they say about rank, and rank is read from the account's
         `runnable` (-Infinity for a frontier whose every member is host-owed), not from the step code. An unknown code is
         refused above. Non-blocking: the engine leaves the hot set while its round runs, so a slow reply here
         never stalls another document. */
      rd.shape = "serviced";
      /* The engine's word for which of the two this round was, recorded for `_level1Serviced` and never branched
         on. */
      rd.stepCode = st;
      target.state = "fetching";
      /* `_readyP` answers one question, when this engine becomes rankable again, for both a boot and a service
         round. */
      /* The engine's stall statement travels with the round, because the round may wait on bytes only when the
         engine has nothing runnable; it is relayed, not re-asked. A round with a runnable frontier returns at
         once, so one never-ending reply body cannot keep a document out of the hot set. */
      target._readyP = ops.serviceFetch(target, st === 3).then(
        () => { target.state = "hot"; },
        (e) => {
          /* A throw out of a service round is an invariant failure (every assert in the reply builders, the
             notice router and the request loop lands here): bannered, and rethrown in dev. */
          target.state = "hot";
          crashBanner("service", String((e && e.stack) || e));
          if (self.APICLIENT_DEV) throw e;
        });
      // Then return to the event loop through a macrotask so the one thread services its message port (evals,
      // postMessage, timers) before re-entering and resuming the byte-identical frontier.
      await macroYield();
    }
    } finally { _level1Record(pool, rd); }
  }
}

// ---- Engine-bound ops and the live pool ----
const _pool = [];        // booting/loading/hot/fetching records; one instance per agent cluster, bounded by the RAM floor
/* Documents awaiting a slot: { html, msg, persist, resolve, reject }, with no instance built yet. `html` is the
   field both consumers read (admission hands it to engineCreate, the navigation arm to engineJoin); the message
   carries no document (its entry asserts so), so `msg.pageHtml` is never read from a job. */
const _waiting = [];
/* Addresses an application declared are pages of itself, waiting to be loaded: the third kind of Level-1 work
   item. An engine reached HTML §7.4.4 Non-fragment synchronous "navigations"'s URL and history update steps
   (`history.pushState`/`replaceState`, how a client-side router names a page) and announced the address
   (solver/route_seed.h, the `document.seed` notice); a route the bundle names and no link exposes is otherwise
   never loaded.
   It holds no bytes, so it is its own register: the fetch is an external effect the WFQ carries as a cost, so
   it happens at admission (as a shed cold entry's re-derivation does), not at the notice.
   Keyed by address, which is not a seen-set: a route declared twice while waiting is one work item, an
   admitted address leaves the map and may be declared again, and membership is never refused; the weight
   decides when the fetch is spent. It is in memory only: the declaring document's residue replays its router,
   which declares the route again. */
const _seeds = new Map();   // document identity -> { url, identity, principalUrl, principalOrigin, provenance, … }
/* Declared routes, counted where each question is asked: `declared` at the notice, `landedOnDeclared` when an
   identical declaration was already registered, `landedOnMember` when `admit` found a member holding it (which
   member, in `_seedLandings`), `loaded` when admitted. Within one sample, declared = the other three plus
   `_seeds.size`. */
const _seedCensus = { declared: 0, landedOnDeclared: 0, landedOnMember: 0, loaded: 0 };
const _seedLandings = new Map();   // member label -> declarations consumed by it
self._seedCensus = _seedCensus;
self._seedLandings = _seedLandings;
let _heldRoutes = 0;       // the serial a declared route's load names its declaring document by (`held:route:<n>`)
let _hostDriving = false;
/* A kick that finds a round in flight is an arrival for that round: `_hostArrived` says one came since the
   round's top, and `_hostArrivalWake` resolves the promise its wait arms race. Both are reset by `arrival`. */
let _hostArrived = false;
let _hostArrivalWake = null;
/* The reservation ledger, cumulative because the state it describes is transient: an empty pool after an
   analysis looks the same whether reservations ran or not. Each reservation is counted where made and each
   exit where taken, and the probe asserts the arithmetic (no more booting records than provisionings still
   running, so no phantom reservation holds a cluster).
   `joinedBooting` counts second arrivals for a cluster still being provisioned (the race the reservation
   closes); `joinedRooted` counts arrivals for a running instance (a RESHIP re-delivery, a sub-frame);
   `peakBooting` is the high-water mark of simultaneous reservations. `evicted` and `rehydrated` are the two
   ends of the RAM floor: their difference tells a floor doing its job from one churning (the Level-1 ratchet
   solver/flow.h names, where a resident engine's weight ages by work while a parked item's estimate does not). */
/* `navigated` is its own count, not a kind of join: a navigation adds a document and deactivates the one it
   replaced (HTML §7.4.6.1 Updating the traversable), and folding it into `joinedRooted` would mix link clicks
   into the count of same-origin sub-frames. */
const _reserveStats = { made: 0, rooted: 0, failed: 0, joinedBooting: 0, joinedRooted: 0, peakBooting: 0,
                        evicted: 0, rehydrated: 0, navigated: 0, seeded: 0 };
/* Which live documents these findings belong to. `documentId` is the only document key in this system (a tab
   holds many documents and a (tab, frame) pair is reused across navigations), so the names are carried, not
   re-derived from a sourceUrl. The list is the waiters, the same set the terminal result is resolved to: every
   browser document that joined the instance is answered with the same analysis, so a snapshot belongs to all
   of them. Empty is a positive statement: the instance has no live caller (a child navigable the engine
   announced, a rehydrated recipe), and its findings go to the cumulative store alone. */
function engineLiveDocumentIds(eng) {
  DCHECK(eng._dropped !== true,
         "the live documents of an instance a Clear dropped were asked for — hostClear rejects its waiters, so " +
         "the empty list would read as an instance with no live caller and merge a wiped run's findings to " +
         "the store; the round that is still driving it must refuse a dropped engine first, as streamPartial " +
         "does");
  return eng._resolvers.map((w) => {
    DCHECK(w.msg && w.msg.documentId,
           "a live caller waiting on this instance carries no documentId — astDispatch asserts the browser's " +
           "name for every document it seats, so a waiter without one is a page whose findings have nowhere " +
           "to be merged and would be reported as analysed and empty");
    return String(w.msg.documentId);
  });
}
/* Whether an instance has a live caller is its waiter list and nothing else: an instance created for a child
   navigable or a recipe has none, and a browser document that arrives later for a document it already runs
   (the routing walk in `admit`) gives it one, so finalize answers that document instead of merging alone. */
function engineHasLiveCaller(eng) {
  return eng._resolvers.length !== 0;
}
/* The seat for an admission whose document is still on the network, the fourth pool state. `ops.admit()` runs
   at the top of every round, so a suspension inside it would stall the whole Level-1 loop; a declared route's
   navigation and a shed residue's re-derivation therefore load on a seat, and the round returns. No deadline
   is added: it could only truncate a slow reply.
   A seat answers for no document: its cluster and document id are unknown until the response lands (the
   message's address is the response's after a redirect), so `hostClusterOf` and `hostHolderOf` cannot match
   it, and both kinds mint group ids nothing else keys to (`admitSeatLand` asserts that). It carries the fields
   every pool walk reads without checking state (`_resolvers`, `joinedDocIds`, `_readyP`).
   It never rejects: a throw out of the load is an invariant abort, stored and rethrown by the round that lands
   the seat (so it reaches hostSchedule's failure arm as before), since a rejection here would be unhandled on
   every round that does not wait on it. */
function admitLoadSeat(what, load) {
  DCHECK(what !== null && typeof what === "object" && (what.kind === "seed" || what.kind === "cold"),
         "an admission seat was taken for a work item this zone has no landing arm for — `admitSeatLand` " +
         "dispatches on that word, so a seat outside the two kinds is a slot that blocks admission and can " +
         "never be finished by anybody");
  DCHECK(typeof load === "function",
         "an admission seat was taken with no load to perform — the seat exists so the round can RETURN while " +
         "a document arrives, so one with nothing arriving is a slot nothing will ever clear");
  DCHECK(typeof what.identity === "string",
         "an admission seat was taken with no identity of the document it loads — the seat is that document's " +
         "frontier member until it lands, and a member with no identity is one a declaration cannot land on");
  const seat = { state: "loading", what: what, identity: what.identity, landed: null, failed: null, settled: false,
                 /* Absent by value rather than omitted, so pool walks that do not check state read `null`
                    and never match a seat (`hostHolderOf`, `hostClusterOf`, `_bestCandidate`, hostClear). */
                 cluster: null, docId: null, topDocId: null, joinedDocIds: [], msg: null, r: null,
                 _resolvers: [], _readyP: null };
  seat._readyP = (async () => {
    try { seat.landed = await load(); }
    catch (e) { seat.failed = e; }
    finally { seat.settled = true; }
  })();
  _pool.push(seat);
  return seat;
}
/* The landing: every decision about the loaded document, made on a round. It runs at the top of `admit`
   ahead of the candidate order, because the seat's fetch is already spent (the order chose the item and the
   request went out), as the seed arm's unconditional `_seeds.delete` reflects. It returns `null` for the
   census: this round spent its advance on the admission and never asked the non-resident order. */
async function admitSeatLand(seat) {
  const i = _pool.indexOf(seat);
  DCHECK(i >= 0,
         "an admission seat was landed while no longer in the pool — the pool is the register of who holds " +
         "what and this is the only thing that takes a seat out of it, so a seat that is not there is one a " +
         "Clear removed and whose document is about to be seated into a session the person asked to forget");
  _pool.splice(i, 1);
  /* The seat settled exactly one way: both outcomes take opposite arms and `null` is a legitimate value of
     neither, so a seat with both or neither is asserted. */
  DCHECK(seat.settled && ((seat.landed === null) !== (seat.failed === null)),
         "an admission seat was landed in a state it cannot be in (settled=" + seat.settled + ", landed=" +
         (seat.landed === null ? "absent" : "present") + ", failed=" +
         (seat.failed === null ? "absent" : "present") + ") — the load stores exactly one of the two and the " +
         "round lands the seat only after `settled`, so anything else is a document about to be built out of " +
         "an answer nobody gave");
  if (seat.failed !== null) throw seat.failed;   // relayed, not swallowed: the round owns this failure (see admitLoadSeat)
  if (seat.what.kind === "seed") {
    const seed = seat.what.seed;
    /* The seed's own navigation through the one chokepoint. Both principals and the provenance are the
       declaring document's, taken when the route was declared and carried on the work item, since the engine
       that declared it may be gone. */
    /* The bytes arrived on an earlier round (see admitLoadSeat) and are read here. */
    const loaded = seat.landed;
    /* The refusals a seeded document owes its reader, each costing one fetch at most: a decline (no request
       made; this caller states `report`), an unavailable load, a response that landed on another origin (a
       Document of origin B in a cluster keyed on A), and an empty body (an ordinary empty Document, but not a
       bundle to explore). The seat is already out of the pool and the `_seeds` entry was removed when picked,
       and the declaring document's residue declares the route again. The decline is tested first: it carries
       `bytes: null`, so a same-origin declined route would otherwise reach `loaded.bytes.length`. The reason is
       printed verbatim, since `blocked-signal:` names a row the person can widen while `blocked-destructive:`
       is permanent. */
    DCHECK(loaded.declined === null || typeof loaded.declined === "string",
           "a declared route's load does not STATE whether it was declined — `loaded.declined` is `" +
           String(loaded.declined) + "`. Every arm of `navigationLoad` states it, and a `||`-shaped read here " +
           "would make a route the chokepoint REFUSED indistinguishable from one it fetched: the three " +
           "disjuncts beside it are all false for a declined same-origin route, so the seat would fall " +
           "through to `loaded.bytes.length` and die reading `length` off null");
    const _landed = originOf(loaded.url);
    if (loaded.declined !== null || loaded.unavailable !== null || _landed === "" ||
        _landed !== originOf(seed.principalUrl) || loaded.bytes.length === 0) {
      console.warn("[bridge] a declared route could not be seeded: " + seed.url + " — " +
                   (loaded.declined !== null ? "the chokepoint DECLINED it on `" + loaded.declined + "`, so no " +
                      "request was made; the rule that refused it is named in that token"
                    : loaded.unavailable !== null ? JSON.stringify(loaded.unavailable)
                    : _landed !== originOf(seed.principalUrl) ? "landed cross-origin at " + (_landed || "an unparseable URL")
                    : "the response carried no bytes"));
      return null;
    }
    /* A cluster of one: the declared page is not nested in the declaring document and nothing holds a
       WindowProxy for it, so it is a top-level traversable in a group this zone mints. The principal is the
       declaring document's browser-stated origin carried on the work item, never `originOf(url)` (a sandboxed
       document's address is ordinary while its origin is opaque); the addresses were compared above.
       `credentialed` is the load's own answer, so it states the load that happened. `topLevelUrl` is its own
       address after redirects (HTML §8.1.3.1, §7.5.1's `creationURL`). */
    /* The provenance carried into the residue this run parks: the load was decided from `seed.provenance`, so
       the stored grade must agree (at a widened origin a forced declaration passes `_seedRefusal`). */
    const _seedDoc = seed.doc;
    const msg = { type: "AST_ANALYZE", sourceUrl: loaded.url, origin: principalOf(_seedDoc, seed.principalOrigin),
                  groupId: "seed:" + (_nextSeedGroup++), responseHeaders: loaded.headers,
                  topLevelUrl: loaded.url,
                  credentialed: loaded.credentialed,
                  /* The declaring instance's record joined with the declaration's words, as for a child
                     navigable. */
                  provenance: _seedDoc.reach, arrival: docRecordArrival(_seedDoc),
                  sandbox: docRecordSandbox(_seedDoc),
                  /* No creator, so a new container (Local Network Access §3.5): its response's space. */
                  addressSpace: self.safeFetchDocumentAddressSpace(loaded.url, null),
                  persist: true };
    DCHECK(hostClusterOf(clusterKeyOf(msg)) === null,
           "a declared route minted a browsing-context group this pool already runs an instance for — the " +
           "group id is a fresh counter, so a hit means two seeds were given one id and the second would " +
           "JOIN the heap the first built, which is two documents behind one agent");
    _reserveStats.seeded++;
    /* The load's Set-Cookie fields go to the jars their URLs' origins key in this document's group. */
    await zoneRouteSetCookies(msg.groupId, loaded.setCookies);
    /* `null` creator: nothing embedded or opened it, so HTML §7.1.7 has no container to clone. `u`/`null`/
       `none`: no parent, no container element and no ancestors, one fact about a top-level page. `cold: true`:
       no caller awaits it, so its findings merge into the cumulative store. Not referenced: nothing holds a
       WindowProxy for a merely declared route, so its frontier may drain. */
    await engineCreate("", loaded.bytes, msg, true, null, loaded.url, null, "u", "N", "null",
                       "none", msg.sandbox, 0)._readyP;
    return null;
  }
  DCHECK(seat.what.kind === "cold",
         "an admission seat reached the landing with a kind neither arm above builds — `admitLoadSeat` " +
         "refuses any word but the two, so a third here is a kind added at the seat and not at the landing, " +
         "and the document would be dropped with its fetch spent and nothing said");
  {
    /* The whole parked document comes back through frontierDoc, the reader paired with the park's writer, and
       the recipe's flows resume inside it. Content is read for the picked item only (the ranking view reads two
       numbers), and since that read suspends it happened on the seat, one round earlier. */
    const stored = seat.landed.stored;
    DCHECK(stored,
           "the cold tier's ranking view named an entry the store does not hold — this zone is the store's " +
           "only writer and frontierPut updates the view on its way through, so a row with no entry is the " +
           "projection and the store having drifted apart");
    /* A shed residue's document is on the network, so it was fetched back on the seat (the same request the
       shed was judged against), never awaited inside admission, where a body that never ends would freeze the
       Level-1 loop. A failed re-derivation strands the entry and seats nothing; a stranded row is not offered
       again (see `_bestCandidate`), so the failure costs one fetch in total. */
    let doc;
    if (stored.shed) {
      const back = seat.landed.back;
      if (!back) return null;
      doc = frontierDoc({ key: stored.key, sourceUrl: stored.sourceUrl, topLevelUrl: stored.topLevelUrl,
                          origin: stored.origin, responseHeaders: back.headers, html: back.bytes, code: "",
                          recipes: stored.recipes, value: stored.value, pulls: stored.pulls, at: stored.at,
                          credentialed: stored.credentialed, provenance: frontierProvenance(stored),
                          arrival: frontierArrival(stored), sandbox: frontierSandbox(stored),
                          addressSpace: frontierAddressSpace(stored) },
                        "was re-derived for the cold tier");
    } else {
      if (seat.landed.body === null) return null;   // release: frontierBody asserted the reference in dev
      const inmem = Object.assign({}, stored, { html: seat.landed.body });
      delete inmem.doc;
      delete inmem.docBytes;
      doc = frontierDoc(inmem, "came back from the cold tier");
    }
    /* The principal resumes with the recipe: a parked flow resumes into the same world only under the same
       principal, which cannot be re-derived from the address. The stamp site refuses an empty one. */
    /* A resumed recipe is a cluster of one: the group it parked in is gone, so its group is `cold:` plus its
       frontier key (address|identity|bundle, unique per recipe, never a tab id), which also lets an empty principal
       collide with nothing. */
    const msg = { type: "AST_ANALYZE", pageHtml: doc.html, code: doc.code, sourceUrl: doc.sourceUrl,
                  origin: doc.origin, groupId: "cold:" + seat.what.key,
                  responseHeaders: doc.responseHeaders,
                  topLevelUrl: doc.topLevelUrl, credentialed: stored.credentialed,
                  /* The word this residue was parked under, re-stated so the entry it parks again keeps it. */
                  provenance: frontierProvenance(stored), arrival: frontierArrival(stored),
                  sandbox: frontierSandbox(stored), addressSpace: frontierAddressSpace(stored), persist: true };
    /* A rehydration whose engine aborts is bannered by engineBootFailed at the reservation, with the pool slot
       it releases; no catch here. A rehydrated recipe never has a caller (`cold`). */
    /* The stored document is passed as parked; engineRoot asserts its shape, so a missing document is never
       read as an empty page. */
    _reserveStats.rehydrated++;
    /* `null` creator: a rehydrated recipe's document had no creator in this session. */
    /* `u` and `none`: the frontier key names a document with no embedder here; a parked child navigable resumes
       through the create notice its creator's replay re-emits, which carries its parent and ancestors. */
    /* Not referenced: the recipe's peers are gone, so no instance holds a WindowProxy for the rebuilt document;
       a child it creates again arrives as a create notice and is provisioned referenced. This differs from
       `qjs_set_referenced` surviving a teardown, which is one instance's statement about itself across a park;
       here a new instance replaces one whose peers no longer exist. */
    await engineCreate(doc.code, doc.html, msg, true, null, null, null, "u", "N", "null",
                       "none", msg.sandbox, 0)._readyP;
    return null;
  }
}
const _hostOps = {
  key: engineKey,
  arrival: () => {
    _hostArrived = false;
    return new Promise((resolve) => { _hostArrivalWake = resolve; });
  },
  /* The tree's accounts over the population the round ranks: admission builds the cold index on its first line,
     so it exists by the time the pick asks. */
  tree: () => {
    DCHECK(_frontierIndexBuilt,
           "the Level-1 tree was asked for before the cold tier's index was built — admission builds it first " +
           "in every round, and a tree without the parked rows ranks every served document as newer than it is");
    return level1Tree(_pool, _frontierIndex);
  },
  /* Which resident engine must give up its RAM, asked of the one order: admission asks it with room, this at
     the floor, and rehydration is a candidate that is not resident. The gate is the floor, not
     `_admissionHasHeadroom`: a reservation in flight blocks admission but is no reason to evict.
     Strictly greater, so a tie leaves the incumbent (solver/flow.c's flow_pick makes the same comparison), and
     equal items never pay a park and a rehydration to swap places. A resident engine and a parked item are
     weighed by one function over one account, so a parked document re-enters at the rank its service earned. */
  /* It answers `evict` (the engine to give up its memory, or null) and `cand` (the reading of the non-resident
     order it decided against, or null), together, because this is where the Level-1 comparison
     `level1Ahead(pick.best.key, worst)` happens and the census must see it. hostSchedule counts the asks, since
     admission may also have asked this round. */
  evictee: async (hot) => {
    if (!_atRamFloor()) return { evict: null, cand: null };   // there is room: nothing has to give anything up
    const idx = await frontierIndex();
    const pick = _bestCandidate(_candPopulation(idx));
    if (!pick.best) return { evict: null, cand: pick.census };   // nothing is waiting for the memory
    const tree = level1Tree(_pool, idx);
    let worst = null, worstK = null;
    for (const e of hot) {
      const k = engineKey(e, tree);
      if (!worst || level1Ahead(worstK, k)) { worst = e; worstK = k; }
    }
    // every live engine is mid-round; the floor is re-asked next round
    if (!worst) return { evict: null, cand: pick.census };
    if (!level1Ahead(pick.best.key, worstK)) return { evict: null, cand: pick.census };
    _reserveStats.evicted++;
    return { evict: worst, cand: pick.census };
  },
  /* The cold-tier park, not caught: main.c's qjs_request_park DCHECKs `g_begun` and calls engine_request_park,
     which raises `g_park_req`; engine_sched_slice takes it at the next step boundary, writes every member
     through cold_park and answers ENGINE_STEP_DONE. */
  requestPark: async (eng) => { await eng.r.renderer.requestPark(); },
  /* Incremental merge on a coarse cadence: snapshot a hot engine's findings and merge them into the cumulative
     store without waiting for a finalize an unbounded engine may never reach. First sight starts the clock, so
     an analysis finishing within PARTIAL_MS never pays for it. qjs_emit_partial appends a fresh @RESULT to
     eng.lines; this parses that snapshot, consumes the line (finalize asks for its own), and merges through
     onFrontierAdvance (globalStore, idempotent). */
  streamPartial: async (eng) => {
    /* A retired instance is asked nothing more; a retirement can land at any await of the round. */
    if (eng._crashed) return;
    const now = Date.now();
    if (!eng._lastPartial) { eng._lastPartial = now; return; }
    if (now - eng._lastPartial < PARTIAL_MS) return;
    eng._lastPartial = now;
    /* An engine abort is the engine crashing; renderer-host's own asserts reject too and are this zone's
       contract, hence RETHROW_FATAL. The scan below runs after the call answers, since its @RESULT rides that
       reply. */
    try { await eng.r.renderer.emitPartial(); }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "partial", e); return; }
    /* A Clear landed during the one await: the snapshot is of a page the person asked to forget, and hostClear
       has rejected its waiters (engineLiveDocumentIds would abort on them), so it is not merged. The frame is
       not taken here; the release step at the bottom of hostSchedule owns it. */
    if (eng._dropped || eng._crashed) return;
    let idx = -1;
    for (let i = eng.lines.length - 1; i >= 0; i--) if (String(eng.lines[i]).startsWith("@RESULT ")) { idx = i; break; }
    /* qjs_emit_partial prints one result unconditionally, so an answer without one is refused. */
    if (idx < 0) { engineRefuse(eng, "qjs_emit_partial answered with no @RESULT line"); return; }
    const partial = linesToAnalysis([eng.lines[idx]], eng.msg, "partial", eng);   // parse only the snapshot line
    eng.lines.splice(idx, 1);                                           // consume it
    /* A refused snapshot is the instance's output failing this zone's door, so the instance is retired as
       crashed with the refusal as its cause (the step op finalizes a crashed instance); the person sees a crashed
       run, never a snapshot read off a record that has no document. */
    if (partial._run === "crashed") { engineRefuse(eng, partial.resolverErrors.at(-1).message); return; }
    // Merge on any surface: an XSS-only page (verified @S PoCs, no endpoints) must surface incrementally too.
    /* `securitySinks` is on every accepted document (resultDocumentRefusal), and a snapshot built from the @RESULT
       line above always has one, which is asserted rather than assumed. */
    DCHECK(analysisHasDocument(partial),
           "an incremental snapshot carried no engine document — it is parsed from the @RESULT line this " +
           "function just found, so its absence is that parse having produced something else entirely and " +
           "every finding in the snapshot is about to be read off a record that has none");
    /* Page errors are the third surface, the only one a page that learned nothing has: HTML §8.1.4.4 Calling
       scripts reports an uncaught exception rather than propagating it, so each one ends a program and names a
       capability the page reached for, and a run that never drains never reaches a terminal record. Asserted,
       not defaulted: linesToAnalysis builds the array on both arms. */
    DCHECK(Array.isArray(partial.resolverErrors),
           "an incremental snapshot carried no resolverErrors array — linesToAnalysis builds one on every " +
           "arm it has, so its absence is that composition changed under this reader and every uncaught " +
           "throw the engine recorded for this page would be discarded with the snapshot that carries it");
    const hasWork = partial.fetchCallSites.length || partial.securitySinks.length ||
                    partial.resolverErrors.length;
    if (!hasWork) return;
    /* The merge callback is the other half of this edge: offscreen-brain.js installs it before this file
       loads, so its absence is a broken load order. */
    DCHECK(typeof self.onFrontierAdvance === "function",
           "the trusted zone has no onFrontierAdvance to merge an engine's findings into — every incremental " +
           "finding this engine emits has nowhere to go");
    self.onFrontierAdvance(eng.msg.sourceUrl, engineLiveDocumentIds(eng), partial, eng._epoch);
  },
  step: async (eng) => {
    let st;
    /* An instance this zone retired (a refused result) is finalized rather than stepped again. */
    if (eng._crashed) return 0;
    /* The identities other instances of this cluster emitted since this one was last handed its partition. */
    try { await noveltyHand(eng); }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "novelty", e); return 0; }
    bootSpanMark(eng.docId, "firstStep");
    try { st = (await eng.r.renderer.step()).code; }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "step", e); return 0; }   // crashed instance: finalize it (loud) rather than keep stepping a dead engine
    /* Retired while the step was in flight: its answer is not read. */
    if (eng._crashed) return 0;
    /* The code is the instance's statement, one of ENGINE_STEP_DONE (0), ENGINE_STEP_YIELD (2) and
       ENGINE_STEP_STALLED (3). Any other is refused before anything is recorded from this step, so the instance
       keeps the facts of its last accepted round and is retired through DONE. */
    if (st !== 0 && st !== 2 && st !== 3) {
      engineRefuse(eng, "qjs_step answered the code " + JSON.stringify(st) + ", which is none of DONE (0), " +
                        "YIELD (2) and STALLED (3)");
      return 0;
    }
    /* The round ends here, so the re-rank reads what this step left: the frontier it advanced and the memory it
       grew. A crashed instance is not asked; the branch above hands it to finish(). */
    /* A refused reply retires the instance now, through DONE. */
    if ((await engineRecordFacts(eng)) !== null) return 0;
    return st;
  },
  /* One service round, with no second op: engineServiceFetch ends by servicing host requests, so a separate
     requests-only op would let a caller skip the replies. */
  /* A service round records too: a delivered reply body (possibly a whole bundle) grows linear memory with no
     step in between, and the replies resume parked flows, so the RAM floor and the ranking must see both
     before the next pick. A round that threw records nothing; hostSchedule's rejection arm owns it. */
  /* It owns a cleared engine's frame: renderer-host refuses to destroy a renderer with a call outstanding, so
     hostClear marks the engine and the round removes the frame when it lands, on both arms, since a round that
     threw leaves the same iframe behind. */
  serviceFetch: async (eng, stalled) => {
    try { await engineServiceFetch(eng, stalled); await engineRecordFacts(eng); }
    finally { if (eng._dropped) engineFrameDestroy(eng); }
  },
  /* The round's half of a Clear: give back the frame the Clear could not take, and nothing else. No result is
     asked for, no residue persisted (the Clear emptied the frontier), and no waiter answered (hostClear
     rejected them with "cleared", which _dispatchDocument reads to abandon its tail). */
  release: async (eng) => {
    DCHECK(eng._dropped,
           "the scheduler was handed an engine that left the pool without a Clear having dropped it — the pool " +
           "is spliced by this loop's own `finish` and by hostClear and by nothing else, so a third remover is " +
           "an instance being reclaimed by two owners and its frame freed twice");
    DCHECK(eng.r.outstandingCalls() === 0,
           "a cleared engine's round landed with " + eng.r.outstandingCalls() + " call(s) still outstanding — " +
           "this is the round that owns the frame, so a call still in flight here is a second op driving an " +
           "instance the user wiped, and the teardown below would abort inside renderer-host instead");
    engineFrameDestroy(eng);
  },
  /* Admission, the one order asked where there is room (evictee asks it where there is not), in two parts:
     (1) Routing, which costs no RAM and is neither ranked nor gated: a waiting document whose agent cluster
         already has an instance joins it, and a sub-frame waits for its embedder to name it.
     (2) Admission over the one candidate order (_bestCandidate): a waiting document, a declared route and a
         parked frontier compete by value, and only the RAM floor holds an item back. */
  admit: async () => {
    /* The one suspension in this function, taken first, so the routing walk, the pick and engineCreate's
       synchronous slot-taking run in one turn and the pool is an accurate register when consulted. */
    const _coldRanking = await frontierIndex();
    /* A seat whose load has settled is seated before anything else, because its fetch is spent; taking it
       first also keeps the wait arm from spinning on a `_readyP` that resolves immediately. One per round, like
       every advance. */
    const _landing = _pool.find((e) => e.state === "loading" && e.settled);
    if (_landing) return admitSeatLand(_landing);
    /* An index walk, not a shift, because one waiting document can be legitimately not yet seatable. */
    let i = 0, swap = null;
    while (i < _waiting.length) {
      const job = _waiting[i];
      /* One instance per agent cluster, asked of the pool: a page and its same-origin iframe are one
         similar-origin window agent in one heap. */
      const key = clusterKeyOf(job.msg);
      const cluster = hostClusterOf(key);
      const docId = String(job.msg.documentId);
      /* Browser-set: frame 0 is the group's top-level traversable. `sender.frameId` comes from the browser
         process (as `sender.tab.url` does), not from a frame's claim about itself (SECURITY.md). */
      const isTop = !job.msg.frameId;
      if (cluster) {
        /* The cluster's instance already holds this document: a same-origin sub-frame is created inside that
           heap by HTML §4.8.5's insertion steps (navigable.c names it, and HTML §7.4.5 loads it through the same
           chokepoint), so its caller is answered by that instance's finalize, as a RESHIP re-delivery is. A
           reservation answers exactly as an instance does, since the pool slot is taken before the first await;
           `joinedBooting` counts that case. */
        /* The group's top was replaced: a same-origin navigation in a tab whose cluster has an instance. Both
           halves are one navigation in HTML §7.4.6.1 Updating the traversable's order: the incoming Document is
           created first (engineJoin) and the outgoing one deactivated after (engineUnload). This is not §7.3.1.6
           Navigable destruction: the navigable survives. A COOP navigation that switched groups (HTML §7.1.3.2)
           does not reach here; its new group gives it a cluster key no instance holds. */
        if (isTop && cluster.topDocId !== docId) {
          /* One per round, as for admission: a join copies a whole document and an unload seeds a task on every
             timeline, so it is an advance. Later ones stay in `_waiting`, which also keeps two navigations of
             one tab in order. */
          if (swap) { i++; continue; }
          /* Recorded and spliced synchronously, before anything suspends: `topDocId` is what the next arrival
             consults, so a second navigation cannot record a swap from the already-replaced document. */
          swap = { cluster, job, outgoing: cluster.topDocId, docId };
          cluster.topDocId = docId;
          cluster._resolvers.push(job);
          _waiting.splice(i, 1);
          _reserveStats.navigated++;
          continue;
        }
        /* Every other arrival for a cluster with an instance is a document it already runs (a same-origin
           sub-frame realm, or one document delivered twice); a top that is not the current top was taken above,
           so reaching here with one is asserted. */
        DCHECK(!isTop || cluster.topDocId === docId,
               "the TOP document of a browsing-context group arrived for a cluster whose instance holds a " +
               "different top (" + cluster.topDocId + " vs " + docId + ") and the navigation branch above did " +
               "not take it — that branch is the ONLY thing that may change `topDocId`, so this is either a " +
               "second writer for that field or an `isTop` computed two different ways in one function, and " +
               "attaching this caller here reports the page that was replaced");
        cluster._resolvers.push(job);
        _waiting.splice(i, 1);
        /* No instance installs this arrival's bytes unless the cluster already holds the document by its own
           name (a re-delivery of its root, which settles the entry itself). */
        if (hostHolderOf(docId) === null) documentPolicySettle(docId, null);
        if (cluster.state === "booting") _reserveStats.joinedBooting++; else _reserveStats.joinedRooted++;
        continue;
      }
      /* A sub-frame never roots a cluster: its embedder creates it (HTML §4.8.5's insertion steps name it in
         the embedder's engine), and the engine either runs it as a realm in its own heap (same-origin,
         `child_in_this_agent`, no notice) or announces it, and the announcement roots or joins its cluster
         under the engine's name. A content script's arrival for the same document is a second route to it and
         may not name it: the engine interns documents by name, so two names for one document are unroutable.
         The browser's `documentId` joins the cluster (above) instead. `_isRealOrigin` exempts the opaque case: a
         sandboxed sub-frame's per-document principal is something no embedder's notice can state (the create
         arm's residual), so it is a cluster of one and roots here. */
      if (!isTop && _isRealOrigin(job.msg.origin)) { documentPolicySettle(docId, null); i++; continue; }
      i++;
    }
    /* (1b) The navigation is performed here, outside the walk, because it suspends and the walk must not: the
       walk decides synchronously (moving `topDocId`, attaching the caller) and this performs, then returns,
       since this round's advance is spent. It is not RAM-gated: it adds a document to an existing instance and
       destroys the one it replaced. The incoming Document has no creator (HTML §7.1.7 Policy containers), so the
       empty container pair is the positive statement, as for a reported root. */
    if (swap) {
      /* A cluster may still be booting when the tab navigates, so the navigation waits for it: `qjs_join` needs
         `qjs_init` and `qjs_begin` to have run, which `_readyP` settling means. */
      if (swap.cluster.state === "booting") await swap.cluster._readyP;
      DCHECK(swap.cluster.state === "hot" || swap.cluster.state === "fetching",
             "a tab navigated in a cluster whose instance never became one — the reservation holding it failed " +
             "to boot, so there is no agent for the incoming document to join and nothing to unload the " +
             "outgoing one out of; the caller was attached to that reservation and has already been answered " +
             "with the crash record by engineBootFailed, which is why this is a state to assert and not one " +
             "to report");
      /* Release path under the assert: the record is out of the pool and its callers answered. */
      if (swap.cluster.state !== "hot" && swap.cluster.state !== "fetching") return;
      const _tlu = swap.job.msg.topLevelUrl || swap.job.msg.sourceUrl;
      DCHECK(typeof _tlu === "string" && _tlu !== "",
             "a navigated top-level document arrived with no address to be its own TOP-LEVEL CREATION URL — " +
             "HTML §8.1.3.5 reads it to decide whether this realm is a secure context, so Web IDL §3.3.13's " +
             "members would exist or not by a guess; a top-level traversable's is its own document address, " +
             "which is the one thing every arrival at this zone carries");
      /* No creator, so no container to clone: this Document's container comes from its own response. The empty
         CSP pair states that; the embedder policy has no empty spelling and states the section's new policy. */
      /* The parent is `u`: what joins here is a top-level traversable's incoming Document (HTML §7.4.6.1
         Updating the traversable), nested in nothing, and HTML §3.1.3's list is `none` for the same reason. */
      /* The bytes are on the job, not its message (the job table's only writer puts them there, and the message's
         entry asserts it carries no document), as for the admission arm's engineCreate. */
      await engineJoin(swap.cluster, swap.job.html, swap.job.msg, swap.docId, _tlu,
                       { csp: "", cspReportOnly: "", selfOrigin: "", embedder: NEW_EMBEDDER_POLICY,
                         referrerPolicy: DEFAULT_REFERRER_POLICY }, "u", "N", "null", "none",
                       swap.job.msg.sandbox);
      await engineUnload(swap.cluster, swap.outgoing, swap.docId);
      /* `null`: this round spent its advance on the navigation and never asked the non-resident order. */
      return null;
    }
    /* (2) The one admission over the one order: a waiting tab, a declared route and a parked frontier differ only
       by weight. One per round, the Level-1 loop's shape (rank, advance, re-rank), not a budget: seating a backlog
       in one call would rank it against a working set measured before any of it existed and hold the pool
       through sequential boots, and re-asking per round re-reads the floor. It also keeps a failing rehydration
       (which re-enters the order through engineBootFailed) from spinning inside one call. */
    /* `pick` is null where the order was not asked (no headroom: a reservation, a loading seat, or the floor,
       where evictee asks instead), and `pick.best` is null where it was asked and had no members; the census
       carries the two facts apart. */
    /* A declaration whose document is already a frontier member, hot or cold, is that member: it adds no load,
       and the member's frontier carries the work. It leaves the register here, counted with the member. */
    if (_seeds.size) {
      const _members = _frontierMembers(_candPopulation(_coldRanking));
      for (const [id] of [..._seeds]) {
        const on = _members.get(id);
        if (on === undefined) continue;
        _seeds.delete(id);
        _seedCensus.landedOnMember++;
        _seedLandings.set(on.label, (_seedLandings.get(on.label) || 0) + 1);
      }
    }
    DCHECK(_seedCensus.declared === _seedCensus.landedOnDeclared + _seedCensus.landedOnMember +
                                    _seedCensus.loaded + _seeds.size,
           "the declared routes do not add up: " + JSON.stringify(_seedCensus) + " with " + _seeds.size +
           " registered — every declaration is counted once at the notice and leaves the register one way");
    const pick = _admissionHasHeadroom() ? _bestCandidate(_candPopulation(_coldRanking)) : null;
    const cand = pick ? pick.best : null;
    if (cand) {
      DCHECK(CAND_KINDS.some((k) => k.kind === cand.kind),
             "the admission order produced a candidate of a kind this zone has no way to build (`" +
             cand.kind + "`) — every work item that is not resident is one of the kinds CAND_KINDS declares " +
             "(a document waiting for an instance, an address an application declared is a page of itself, a " +
             "parked frontier), and one outside that table would be silently skipped by whichever of the " +
             "arms below happened to be the fallback");
      /* A declared route (HTML §7.4.4's URL and history update steps ran and the engine announced it,
         solver/route_seed.h). Its one fetch is spent here, because the order chose it ahead of every other work
         item. It leaves the register first and unconditionally: a failed load has still spent the fetch, and
         the declaring document's residue declares the route again. */
      if (cand.kind === "seed") {
        const seed = _seeds.get(cand.id);
        DCHECK(seed !== undefined,
               "the admission order picked a declared route that is no longer declared — `_seeds` is written " +
               "by the notice router and spliced only here, so a pick with no entry is the order and the " +
               "register having parted, and the address about to be loaded would be one nothing named");
        _seeds.delete(cand.id);
        if (!seed) return pick.census;
        _seedCensus.loaded++;
        /* No member holds the document this load makes: the order excluded every declaration one does. */
        const _holder = _frontierMembers(_candPopulation(_coldRanking)).get(seed.identity);
        DCHECK(_holder === undefined,
               "a declared route at " + seed.url + " is about to be loaded while " +
               (_holder && _holder.label) + " already holds that document (" + seed.identity + ") — its " +
               "declaration is that member and adds no load");
        /* The fetch is issued on a seat rather than awaited, so a route whose body never ends cannot freeze the
           Level-1 loop: the seat is a pool member `_admissionHasHeadroom` counts, and the wait arm waits on it
           only when nothing is rankable. Only the network body is off-round; refusals, the message, the cluster
           check and the boot happen in admitSeatLand at the top of a round, so an invariant abort still reaches
           hostSchedule's failure arm. */
        /* The declaring document speaks as itself for this one load, so it ends with the load. */
        const held = "held:route:" + (++_heldRoutes);
        admitLoadSeat({ kind: "seed", seed: seed, identity: seed.identity },
                      () => navigationLoad(seed.url, seed.principalUrl, seed.principalOrigin, seed.provenance,
                                           seed.fromDoc,
                                           /* `report`: a route seed holds no rendezvous and has an arm
                                              (admitSeatLand drops the seat and prints the chokepoint's
                                              reason). A decline is reachable: the notice's `_seedRefusal`
                                              pre-screen asks `safeFetchFiringRefusal`, which is not the
                                              destructive deny list, so a declared `/logout` passes it and
                                              the chokepoint refuses `blocked-destructive:<token>`. */
                                           /*refusalArm*/"report",
                                           /* `tool`: HTML §7.4.4 Non-fragment synchronous "navigations"'
                                              URL and history update steps set the document's URL and fetch
                                              nothing, so the page sent no request for this address and a
                                              reload of it would. This load is active discovery from the
                                              learning document: same-origin with it (the notice router
                                              refuses any other address), and judged by the one egress rule
                                              as any request this tool composes. The declaration's drive
                                              mark travels as the engine stated it. */
                                           /*actor*/"tool", seed.driven, seed.pinned, DOCUMENT_REQUEST_UNSTATED,
                                           /* A declared route is a document only this zone holds. */
                                           async () => ({ identity: await engineIdentity(stampOrigin(seed.principalOrigin),
                                                                                   seed.principalUrl, held),
                                                          referrer: "none", policy: null, request: null }),
                                           docRecordOpaque(seed.doc))
                        .finally(() => speakAsDocumentEnded(held)));
        return pick.census;
      }
      if (cand.kind === "doc") {
        const job = cand.job;
        const key = clusterKeyOf(job.msg);
        const j = _waiting.indexOf(job);
        DCHECK(j >= 0,
               "the admission order picked a waiting document that is no longer waiting — `_waiting` is only " +
               "spliced by this function and by hostClear, and a job admitted twice is one document answered " +
               "by two instances and one caller resolved by whichever finishes first");
        _waiting.splice(j, 1);
        /* The job is attached to the reservation before anything suspends, so the boot's failure path
           (engineBootFailed: banner once, answer every attached caller with the crash record, destroy the frame,
           leave the pool) is the one owner of every caller. An invariant abort from creation (init's return
           code, the bundle id, the key's address) is this zone's contract breaking and travels through
           `_readyP` to hostSchedule's failure arm. */
        /* `null`: a document a content script reported is a root, with no creator to clone a container from. */
        /* `u` and `none`: it is the tab's document, a top-level traversable with no parent and no container
           document. */
        /* Not referenced: what is admitted here is a top-level document of a group (the walk leaves cross-origin
           sub-frames waiting for their embedder's create notice), so no instance holds a WindowProxy for it and
           its frontier may drain. A cross-origin child created into this instance later is the join arm's
           question, refused there, since the flag can only be stated before the frontier is seeded. */
        const eng = engineCreate("", job.html, job.msg, job.persist, null, null, null, "u",
                                 "N", "null", "none", job.msg.sandbox, 0);
        DCHECK(hostClusterOf(key) === eng,
               "engineCreate did not leave its reservation in the pool before returning — the whole point of " +
               "the slot being taken synchronously is that the next arrival for this cluster finds it, so a " +
               "pool that does not hold it is the race this reservation exists to close, still open");
        eng._resolvers.push(job);
        await eng._readyP;
        return pick.census;
      }
      /* The store read and a shed entry's re-derivation are issued on one seat, for the seed arm's reason (the
         re-derivation is a remote body); they are one question, since `shed` is a field of the entry and which
         read is needed is known only after the first answers. admitSeatLand makes every decision on a round. */
      admitLoadSeat({ kind: "cold", key: cand.row.key,
                      identity: documentIdentityOf(cand.row.sourceUrl, cand.row.origin, cand.row.credentialed) },
                    async () => {
        const stored = await frontierGet(cand.row.key);
        return { stored: stored, back: stored && stored.shed ? await frontierRederive(stored) : null,
                 body: stored && !stored.shed ? await frontierBody(stored.doc) : null };
      });
    }
    return pick ? pick.census : null;
  },
  finish: async (eng) => {   // fully explored, or self-parked under RAM pressure: persist the residue to the cold tier, then resolve or merge
    /* A reservation is never finalized: only a stepped engine reaches here, and everything below reads an
       instance that has answered (`eng.r`, `eng.lines`, `eng.fkey`). */
    DCHECK(eng.state === "hot",
           "an engine in state `" + eng.state + "` was finalized — finalize asks the instance for its one " +
           "result document and then tears it down, and a reservation has no instance to ask, so its " +
           "document would be reported as analysed and empty");
    /* The step that answered DONE may have emitted notices before its frontier drained (a flow declares a route
       and ends), and only a service round drains them, which a DONE step never gets. They are acted on here,
       while the instance is still a pool member, as a round would. A synchronous request needs a parked flow,
       so none can be owed by a drained frontier. A crashed instance is not asked, as engineFinalize does not ask
       it for a result: its frame is gone and what it printed is already in `eng.lines`. */
    if (!eng._crashed) {
      for (const line of owedList("GetHostNotices", (await eng.r.renderer.getHostNotices()).notices))
        await hostNotice(eng, line);
      const _owedAtDone = owedList("GetHostRequests", (await eng.r.renderer.getHostRequests()).requests);
      /* A request is a parked flow's, and a drained frontier holds none, so an instance that answered DONE
         while owing one is refused rather than finalized as complete. */
      if (_owedAtDone.length !== 0 && !eng._crashed)
        engineRefuse(eng, "the engine answered DONE while owing " + _owedAtDone.length + " host request(s) (" +
                          JSON.stringify(_owedAtDone.slice(0, 3)) + ")");
    }
    const i = _pool.indexOf(eng); if (i >= 0) _pool.splice(i, 1);
    provisionedEnd(eng, "finalizing");
    zoneReclaimHeldCookies(eng.groupId);
    const result = await engineFinalize(eng);
    provisionedEnd(eng, eng._crashed ? "crashed" : "finalized");
    childRequestsClose(eng);
    /* A crashed run does not write the cross-session frontier: its recipes would claim flows resumable from a
       heap that aborted and its counters a finished run, and with no `_park` the write would delete the residue
       a previous session parked (frontierPut deletes an entry with empty recipes). Skipping it leaves that entry
       as it was. */
    /* The value a parked run adds to its row, read twice over one result: the account the last step stated and
       the result's `_novelty.landed`, both novelty_landed_value with no step between; `_novelty.paid` also counts
       a novel credit made on host time, which no flow was paid, so it bounds them. Both are the engine's
       statements and so input: a run whose readings disagree is refused its park, the entry a previous session
       wrote stays as it was, and the refusal rides the analysis the person is shown. */
    const _parks = eng.persist && result._fkey && result._run === "complete";
    const _nov = _parks ? result._novelty : null;
    const _parkRefusal = (!_parks || (_nov.landed === eng.level1.paid && _nov.landed <= _nov.paid)) ? null
      : "the run's novel value disagrees with its own result (account " + eng.level1.paid + ", landed " +
        String(_nov.landed) + ", paid " + String(_nov.paid) + ")";
    if (_parkRefusal !== null) {
      result.resolverErrors.push({ context: "park-refused", message: _parkRefusal });
      console.warn("[bridge] " + eng.msg.sourceUrl + ": this zone refused the run's park (" + _parkRefusal + ")");
    }
    if (_parks && _parkRefusal === null) {   // persist into the global frontier (the cross-session cold tier)
      /* The gate's premise is another function's: `_run` is `complete` exactly where linesToAnalysis was given a
         document. Were that to break, `recipes` would be "" and frontierPut would delete the residue. */
      DCHECK(analysisHasDocument(result),
             "a run marked `complete` reached the frontier write with no engine document — its `_park` " +
             "recipes and its endpoint count are what this origin's cross-session entry is MADE of, and a " +
             "missing one would erase a previous session's parked residue rather than extend it");
      const prior = result._prior;
      /* The prior entry's account, asserted here because it came from engineRoot's frontierLookup, the one read
         of this store that bypasses frontierIndex's record grammar; the entry written below adds to it. */
      if (prior) level1AccountCheck(prior, "the parked entry this run resumed");
      /* The parked grade, asserted before the record is composed: frontierProvenance reads an absent field as
         `derived` only because every entry a live build writes states a word, which is this line. It is owed to
         the store, whose entries outlive their build, separately from engineRoot's assert, which is owed to the
         fetch closures. */
      DCHECK(eng.msg.provenance === PROVENANCE_OBSERVED || eng.msg.provenance === PROVENANCE_DERIVED ||
             eng.msg.provenance === PROVENANCE_FORCED,
             "a run about to park its residue carries the provenance `" + String(eng.msg.provenance) +
             "` — every composer of an AST_ANALYZE states one (the ambient seed `observed`, a declared route " +
             "and a child navigable the JOIN of the reaching document's grade and their own word, a " +
             "rehydration the word it was parked under), so an absent one is a composer nobody told, and the " +
             "entry it writes would read back as an older store's and be re-fetched as `derived`");
      const _parkProvenance = eng.msg.provenance;
      const _account = engineAccount(eng);
      DCHECK(result._fkey === frontierKeyOf(eng.msg.sourceUrl, eng.origin, eng.msg.credentialed, eng._bundleId),
             "a run is parking under the frontier key " + result._fkey + " while the document it stores is " +
             frontierKeyOf(eng.msg.sourceUrl, eng.origin, eng.msg.credentialed, eng._bundleId) + " — the key " +
             "was composed at the root from the same record, so its identity or bundle moved under the run");
      await frontierPut(result._fkey, {
        /* The recipe carries its environment: the top-level creation URL (HTML §8.1.3.5 Secure contexts decides
           from it which Web IDL §3.3.13 [SecureContext] members exist), the browser-stated principal (a resumed
           instance stamps the origin the parked one had), and the response header list engineRoot relayed to
           qjs_init, which no address can re-derive. */
        key: result._fkey, sourceUrl: eng.msg.sourceUrl, topLevelUrl: eng.msg.topLevelUrl, origin: eng.origin,
        responseHeaders: eng.msg.responseHeaders, html: eng.html, code: eng.code,
        /* `_park` and `fetchCallSites` are guaranteed on this arm (asserted above); a `|| []` here could only
           outlive that guarantee and turn dropped flows into "the recipes joined to ''". */
        /* The provenance of the load that produced this document, read by frontierRederive when it fetches the
           bytes back; bound to a local above so the field's receiver is a name an auditor can anchor. */
        provenance: _parkProvenance, arrival: eng.msg.arrival, sandbox: eng.msg.sandbox,
        addressSpace: eng.msg.addressSpace,
        credentialed: eng.msg.credentialed, recipes: result._park.join(";"),
        /* The document's Level-1 account: the resumed row's plus this instance's, so service and value are
           carried across every park and session. */
        value: _account.value, pulls: _account.pulls, at: _account.at,
        ...(result._parkWhy === "refused" ? { rest: { program: await programDigest(), policy: egressPolicyNow() } }
                                          : {}),
      });
    }
    /* A crashed resume is the finding about that residue: the entry keeps its recipes and records the abort
       under this engine, so the person sees it and no admission re-runs it unchanged. A crash of a fresh boot
       resumed nothing and records nothing on an entry. */
    if (result._run === "crashed" && eng._residueEntry !== null)
      await frontierRecordCrash(eng._residueEntry, rootWhyLine(eng.lines));
    if (!engineHasLiveCaller(eng)) {
      /* No live caller (a child document or a rehydrated recipe): its findings merge into the cumulative store
         here. Not caught, so the merge's own contract asserts (_mergeFrontierResult) are seen. */
      DCHECK(typeof self.onFrontierAdvance === "function",
             "the trusted zone has no onFrontierAdvance to merge a finalized engine's findings into — a child " +
             "document's and a resumed frontier's entire output has nowhere to go");
      self.onFrontierAdvance(eng.msg.sourceUrl, engineLiveDocumentIds(eng), result, eng._epoch);
    }
    /* Every caller waiting on this document is answered; a cold or child engine has none, which is why its
       findings travel on the merge above. */
    for (const w of eng._resolvers) w.resolve(result);
    eng._resolvers.length = 0;
  },
};
/* The scheduler died, and a dead scheduler is not re-entered. Set by the rejection arm below and never cleared:
   every rejection out of this loop is an invariant abort, so the state that killed it is still in `_pool` and
   `_waiting` and the next round would die at the same line. `_hostKicksRefused` counts later kicks, so the pair
   reads "the scheduler is down, and N callers have asked for it since". */
let _hostDead = null;
let _hostKicksRefused = 0;
function _hostKick() {
  /* Dead is asked before driving, so a kick behind an in-flight round is still counted as refused when the
     scheduler is dead (`driving` is about this round, `dead` about every later one). Refused, not retried and
     not asserted: the abort was reported once by the arm that latched it, and asserting here would blame the
     next caller and repeat the abort on every navigation. */
  if (_hostDead) { _hostKicksRefused++; return; }
  if (_hostDriving) {
    DCHECK(_hostArrivalWake !== null,
           "a round is in flight with no arrival latch — every round takes one at its top, before its first " +
           "suspension, so a kick can only find a driving loop after the first was taken");
    _hostArrived = true;
    _hostArrivalWake();
    return;
  }
  _hostDriving = true;
  /* The loop's failure is bannered and, in dev, rethrown as an unhandled rejection: every bridge assertion is
     thrown from inside this loop. Completion and failure take separate arms: re-kicking is what a completed
     round does, while a dying round never reaches `finish`, so `_pool` stays non-empty and a shared re-kick
     would turn one invariant abort into an unbounded retry burying the first report. The failure arm latches
     and stops; the crash is what forces the root fix. */
  hostSchedule(_pool, _hostOps).then(
    /* A completed run re-kicks on the pool, or on an arrival its last round did not ask about (a kick between
       that round's top and its exit). Not on `_waiting` alone: with an empty pool RAM is free, so a document
       still waiting is one admit deferred (a sub-frame whose top has not reported), and that top's CONTENT_SEED
       kicks the pool itself; re-kicking for it would spin. */
    () => { _hostDriving = false; if (_pool.length || _hostArrived) _hostKick(); },
    /* The loop died; nothing retries, because every rejection here is a should-never-happen (an engine abort
       is answered by engineBootFailed and resolves, so one page failing to boot does not take the pool down).
       `_hostDriving` is cleared because no round is in flight; `_hostDead` is what refuses. */
    (e) => {
      _hostDead = e;
      _hostDriving = false;
      crashBanner("host-wfq", String((e && e.stack) || e));
      if (self.APICLIENT_DEV) throw e;
    });
}
/* The offscreen kicks the pool on idle so parked cold recipes are pulled in (admission re-checks the frontier)
   even when no new document arrives, so the one attention keeps advancing across sessions. */
self.kickHostPool = _hostKick;
/* A browser restart resumes the parked frontier by value order with no page visited, so the pool is kicked
   when this document starts, once the cumulative store that a resumed engine's findings merge into is read. */
_globalStoreReady.then(_hostKick);

/* The pool, observed through the boundary it runs behind. renderer-host.js's `rendererProbe` proves the
   transport; this proves the product: the pool builds its instances as renderers, a real page's findings came
   out of one, and frames leave when engines do. It is a read and starts no analysis, since an analysis would
   need a documentId, groupId, origin and frameId, all browser-stated; the analysis it observes is driven from
   outside (`harness goto`). It reports finished as well as live counts, because an empty pool after a clean
   teardown and one that never ran look alike. It cross-checks the registry's routing ids against this
   document's frames, so admitted and merely built instances differ. It is synchronous: every field is a read
   of this realm, so the pool walk and the registry read are one instant. */
self.rendererPoolProbe = function rendererPoolProbe() {
  DCHECK(typeof self.rendererStats === "function",
         "renderer-host.js is not loaded in this zone — it is what obtains every engine's frame, so without " +
         "it the pool has no way to obtain an instance at all and this probe " +
         "would be reporting on an empty document");
  let booting = 0;
  const pool = _pool.map((eng) => {
    /* No engine may carry a Module handle: that would be an untrusted instance in the trusted zone's own realm
       with `HEAPU8` exported, invisible from outside because it analyses pages perfectly well. */
    DCHECK(!("M" in eng),
           "a pooled engine still carries a Module handle — the in-realm path is deleted, so an engine holding " +
           "one is an untrusted instance built in this realm rather than in a sandboxed opaque-origin frame");
    /* A reservation is reported as a pool member, which it is: the record answering which instance holds this
       agent cluster while it boots. Its two Level-1 facts read `null`, because it has stated neither
       (`heapBytes: 0` would admit another engine against RAM about to be spent, and a zero account would
       claim a document that was never served). `joined` counts callers waiting on its findings; `joinedDocIds`
       names the documents the instance holds beyond its root (main.c's `g_joined_ctx`), so a pool of two
       entries can be a page of four documents. */
    /* A loading seat is reported as itself, with no renderer, cluster or document and every Level-1 fact
       absent. It is not counted in `booting`: the arithmetic below is about engineCreate's made/rooted/failed
       ledger, which a seat never enters. */
    if (eng.state === "loading") {
      DCHECK(eng.what !== null && typeof eng.what === "object" && typeof eng.what.kind === "string",
             "a loading seat is in the pool naming no work item — the seat exists so a LATER round can finish " +
             "the admission its fetch was spent on, and one that cannot say which kind of work it is holding " +
             "is a slot that blocks admission and can never be landed by anybody");
      return { name: null, docId: null, topDocId: null, state: "loading",
               framed: false, routingId: null, heapBytes: null, level1: null,
               cold: !engineHasLiveCaller(eng), joined: eng._resolvers.length, joinedDocIds: eng.joinedDocIds.slice(),
               loadKind: eng.what.kind, settled: !!eng.settled };
    }
    if (eng.state === "booting") {
      booting++;
      DCHECK(typeof eng.cluster === "string" && eng.cluster !== "" && typeof eng.docId === "string" && eng.docId !== "",
             "a reservation is in the pool naming no agent cluster or no document — it is answered for by " +
             "both (hostClusterOf for admission, hostHolderOf for routing), so a nameless one is a slot that " +
             "blocks admission while answering nobody");
      return { name: eng.cluster, docId: eng.docId, topDocId: eng.topDocId, state: "booting",
               framed: !!(eng.r && eng.r.frame.parentNode), routingId: eng.r ? eng.r.routingId : null,
               heapBytes: null, level1: null, cold: !engineHasLiveCaller(eng), joined: eng._resolvers.length,
               joinedDocIds: eng.joinedDocIds.slice() };
    }
    /* The working set is read where the pool recorded it (engineRecordFacts takes `workingSetBytes` from the
       reply it awaited); a live engine has reported, because that runs before the record leaves `booting`. */
    DCHECK(eng.r && typeof eng.residentBytes === "number" && typeof eng.r.name === "string",
           "a pooled engine is not backed by a renderer that has reported itself — every instance is obtained " +
           "by rendererLaunch, which does not return until the registry has admitted its agent cluster and the " +
           "frame's invitation acceptance has landed, and engineRecordFacts states its working set before the " +
           "reservation becomes hot");
    /* The routing id is the one field neither this file nor the frame produced: it comes from the renderer
       registry, so it is the evidence that this instance was admitted rather than merely built. */
    DCHECK(typeof eng.r.routingId === "number",
           "a pooled engine's renderer carries no routing id — an id is minted by the renderer registry when " +
           "it decides an agent cluster gets an instance, so an instance without one is a frame this document " +
           "created for itself");
    return { name: eng.r.name, docId: eng.docId, topDocId: eng.topDocId, state: eng.state,
             framed: !!eng.r.frame.parentNode,
             routingId: eng.r.routingId, heapBytes: eng.residentBytes,
             level1: Object.assign({ runnable: eng.level1.runnable }, engineAccount(eng)),
             cold: !engineHasLiveCaller(eng), joined: eng._resolvers.length, joinedDocIds: eng.joinedDocIds.slice() };
  });
  /* No reservation outlives its provisioning, asserted as arithmetic: a reservation is made once and takes one
     exit (rooted or failed), so `made - rooted - failed` provisionings are running and the pool may hold no more
     booting records than that; a phantom would make every later arrival for its cluster wait for ever. `<=`,
     not `===`, because hostClear may splice a reservation while its boot is still running (the provisioning
     removes the frame when it lands). */
  const inFlight = _reserveStats.made - _reserveStats.rooted - _reserveStats.failed;
  DCHECK(booting <= inFlight,
         "the pool holds " + booting + " reservation(s) while only " + inFlight + " provisioning(s) are still " +
         "running (" + _reserveStats.made + " made, " + _reserveStats.rooted + " rooted, " +
         _reserveStats.failed + " failed) — the difference is a slot held by a record whose boot is over, " +
         "which blocks admission forever and answers every later arrival for that agent cluster with a wait " +
         "that never ends");
  /* ── Who decided. `rendererStats()` is what renderer-host.js holds (its `_live` set, checked against the DOM
     above); `getRegistry()` is what render-process-host.js decided, a table keyed by agent cluster whose four
     transitions each assert its arithmetic. The registry is a `Map` in this realm, so this comparison cannot
     tell "the registry decided" from "renderer-host decided and told it"; what it proves is that two
     components' independent records of one set, over two keys, agree, so a frame without an admission, an
     admission whose renderer is gone, or a counter without its slot each shows here. The inversion holds
     structurally: `registerRenderer` has one caller, the only path to a renderer frame. Both reads are
     synchronous in one run-to-completion realm, so nothing interleaves between them. */
  const renderers = self.rendererStats();
  DCHECK(!!self.renderProcessHost,
         "this document holds " + renderers.forked + " forked renderer(s) with no renderer registry in the " +
         "realm — every routing id comes out of that table, so frames existing without it is a document that " +
         "materialized renderers on its own authority");
  const registry = self.renderProcessHost.getRegistry();
  const mine = renderers.routingIds.join(",");
  DCHECK(registry.routingIds === mine,
         "the renderer registry holds renderers [" + registry.routingIds + "] while this document holds " +
         "frames for [" + mine + "] — the two are one set: an id here that is not there is a frame nothing " +
         "admitted, and an id there that is not here is an agent cluster refused an instance forever because " +
         "the renderer holding it is already gone");
  DCHECK(registry.launched + registry.failed === renderers.forked,
         "the renderer registry admitted " + (registry.launched + registry.failed) + " renderer(s) (" +
         registry.launched + " launched, " + registry.failed + " failed) while this document forked " +
         renderers.forked + " — a frame is materialized by one admission and by nothing else, so a difference " +
         "is a frame created outside that path or an admission whose fork never began");
  /* Whether the one loop is alive, which nothing else here shows: a dead scheduler holds what it held when it
     died, so `kicksRefused`, the documents answered by nothing since, tells healthy from wedged. */
  return { renderers: renderers, registry: registry, mojo: self.mojo.stats(),
           scheduler: { alive: !_hostDead, driving: _hostDriving, kicksRefused: _hostKicksRefused,
                        diedOf: _hostDead ? String((_hostDead && _hostDead.message) || _hostDead) : null },
           pool: pool, waiting: _waiting.length, residentBytes: _residentBytes(),
           /* Admissions whose document is still on the network, kept out of `reservations.booting`, which
              accounts for engineCreate's ledger. */
           loadingSeats: _loadingCount(),
           reservations: { made: _reserveStats.made, rooted: _reserveStats.rooted, failed: _reserveStats.failed,
                           booting: booting, inFlight: inFlight, peakBooting: _reserveStats.peakBooting,
                           joinedBooting: _reserveStats.joinedBooting, joinedRooted: _reserveStats.joinedRooted,
                           navigated: _reserveStats.navigated, seeded: _reserveStats.seeded,
                           evicted: _reserveStats.evicted, rehydrated: _reserveStats.rehydrated },
           coldRows: _frontierIndexBuilt ? _frontierIndex.size : null,   // null: the ranking view has not been built yet
           /* The store's decisions, the only observable of residency: documents shed to the share, fetched back,
              stranded (shed and then not fetchable), and `overShare`, the bytes residency refused to shed
              because they were the only copy of a residue. `share` is null until the preference is read, which
              differs from a share of zero. */
           frontier: { shed: _frontierStats.shed, rederived: _frontierStats.rederived,
                       stranded: _frontierStats.stranded, docBytes: _frontierStats.docBytes,
                       overShare: _frontierStats.overShare, share: _frontierShare,
                       defaultShare: frontierDefaultShare() },
           runs: self._engineLog.length, recent: self._engineLog.slice(-4),
           crashes: self._engineCrashOccurred };
};

/* Wipe everything this zone holds or will resume: the Clear button's contract is to stop all work and delete
   all data. Hot engines leave the pool (hostSchedule breaks when it is empty, and an engine out of the pool is
   never stepped, finalized or persisted), and the cold tier is emptied. Waiting documents are rejected with
   "cleared", which _dispatchDocument reads to abandon its tail without recording a page failure. */
async function frontierClear() {
  try {
    await idbWrite("frontier", "default", (s) => s.clear());
    await idbWrite("bodies", "default", (s) => s.clear());
    _frontierIndex.clear();   // the ranking view is the store's projection: a cleared store ranks nothing
    /* And every reply the HTTP cache stored, which would otherwise go on answering sends the person cleared;
       its records' bodies went with `bodies`. */
    httpCacheClear();
    _httpCacheDisk.clear();
    await idbWrite("httpcache", "default", (s) => s.clear());
    /* And the product's known identities, which name what every cleared session found. */
    noveltyClear();
    await idbWrite("novelty", "default", (s) => s.clear());
  } catch (e) { RETHROW_FATAL(e); frontierFail("clear", e); }
}
async function hostClear() {
  const waiting = _waiting.splice(0);
  /* Routes declared this session go with the frontier they were work items on, or the next idle kick would
     fetch a page of an origin the person asked to forget. A declaration has no caller to reject. */
  _seeds.clear();
  _seedCensus.declared = _seedCensus.landedOnDeclared = _seedCensus.landedOnMember = _seedCensus.loaded = 0;
  _seedLandings.clear();
  const dropped = _pool.splice(0);
  _provisioned.clear();
  /* Pairs held for clusters of a session the person asked to forget go with it. */
  _heldCookies.clear();
  for (const job of waiting) job.reject(new Error("cleared"));
  for (const eng of dropped) { for (const w of eng._resolvers) w.reject(new Error("cleared")); eng._resolvers.length = 0; }
  /* The frame goes with the engine: nothing else will reach the renderer of an engine out of the pool, and an
     iframe nobody reaches is a whole WASM instance held resident.
     A round in flight keeps its frame until it lands, and the transport is asked (`outstandingCalls()`), not the
     state: a `hot` engine has a call outstanding during every awaited scheduler op (requestPark,
     step, streamPartial), and destroying it would hit renderer-host's assert and make the Clear itself fail.
     `_dropped` is what the owning round reads on its way out (serviceFetch, hostSchedule's release). */
  /* A reservation keeps its frame too, hence the positive set: a booting engine has no `r` until rendererLaunch
     answers and an ABI call outstanding from then until engineRoot finishes; engineCreate reads `_dropped` when
     its provisioning lands and removes the frame. */
  for (const eng of dropped) { eng._dropped = true; if (eng.state === "hot" && eng.r.outstandingCalls() === 0) engineFrameDestroy(eng); }
  for (const eng of dropped) childRequestsClose(eng);
  /* Every outstanding rendezvous belonged to a dropped instance; left behind, each would hold its asking
     engine's record for the life of the offscreen. */
  _remoteOps.clear();
  /* The run log goes, because it is a list of page addresses, a browsing history (Clear deletes all extension
     data). It is truncated in place: popup-handlers.js asserts this array's presence, and a fresh array would be
     a second one. `_engineCrashOccurred` stays: it holds no page identity, and a wipe must not be able to
     silence a crash nobody has seen yet. */
  self._engineLog.length = 0;
  await frontierClear();
  return dropped.length;
}

self.astDispatch = async function astDispatch(msg) {
  try {
    /* The person's standing egress table is awaited here because the AST_EGRESS_POLICY arm reads it
       synchronously (`safeFetchWiden`, `safeFetchPermit`, `safeFetchPermitted`, `safeFetchEgressTable`,
       `safeFetchWidenedOrigins` each CHECK that it was stated, fatal in release), and none of those is a fetch
       behind the chokepoint's own wait. It is awaited for every type, since which types reach a synchronous read
       is a property of the rest of this function; it is the memoized read started at load, so it costs a
       microtask. */
    await egressPolicyReady();
    /* Six types; any other is an unbuilt capability, so it aborts rather than reporting a false success to a
       caller that will not look. */
    DCHECK(!!msg && (msg.type === "AST_ANALYZE" || msg.type === "AST_CLEAR" ||
                     msg.type === "AST_FRONTIER_SHARE" || msg.type === "AST_EGRESS_POLICY" ||
                     msg.type === "AST_REQUEST_POLICY" || msg.type === "AST_THIRD_PARTY_COOKIES"),
           "the trusted zone dispatched a type this bridge does not answer: `" + (msg && msg.type) + "` — " +
           "every edge into the engine is built here, so an unanswered type is a capability that was asked " +
           "for and never made, not an option the caller may proceed without");
    if (!msg) return { success: false, error: "dispatch with no message" };   // release path under the assert
    if (msg.type === "AST_CLEAR") return { success: true, result: { cleared: await hostClear() } };
    // The frame's CSP verdict on a request the Send panel composed. It is shown, never enforced.
    if (msg.type === "AST_REQUEST_POLICY")
      return { success: true, result: await engineRequestPolicyVerdict(msg.documentId, msg.url) };
    /* This extension's third-party-cookie setting: read, or set by the person, answered with what is now in
       force. `allowed` sets the global setting; `exception` ("allow", "block" or null to clear) sets the
       egress table's `third-party-cookies` row of the subject's site, persisted with the table. A change is a
       person's act, so it carries the user-initiated grade the egress command carries. */
    if (msg.type === "AST_THIRD_PARTY_COOKIES") {
      await _thirdPartyCookiesReady;
      DCHECK(typeof msg.subject === "string",
             "the third-party-cookie command carried no subject — the popup states the origin it is showing");
      if (self.safeFetchWidenable(msg.subject) === null) await siteAsk(siteHostsOf([msg.subject]));
      const _site = self.safeFetchWidenable(msg.subject) === null ? siteOf(msg.subject + "/") : null;
      let _persistError = null;
      if (msg.allowed !== undefined || msg.exception !== undefined)
        CHECK(msg.initiator === PAGE_CONTEXT_USER_INITIATED,
              "the third-party-cookie setting was asked to change by " + JSON.stringify(msg.initiator) +
              " — only the person, in the popup, changes it");
      if (msg.allowed !== undefined) {
        CHECK(typeof msg.allowed === "boolean", "the third-party-cookie setting was asked to become " +
              JSON.stringify(msg.allowed) + ", not a boolean");
        self.identityThirdPartyCookiesStated(msg.allowed);
        _persistError = await prefWriteOutcome(frontierPrefPut("thirdPartyCookies", msg.allowed));
      }
      if (msg.exception !== undefined) {
        CHECK(_site !== null && (msg.exception === null || msg.exception === "allow" || msg.exception === "block"),
              "a third-party-cookie exception " + JSON.stringify(msg.exception) + " was asked for the subject " +
              JSON.stringify(msg.subject) + ", which has no site or is not allow, block or null");
        self.safeFetchSetSiteSetting(_site, "third-party-cookies", msg.exception);
        if (_persistError === null) _persistError = await prefWriteOutcome(egressPolicyPersist());
      }
      return { success: true, result: { allowed: self.identityThirdPartyCookiesAllowed(), persistError: _persistError,
                                        defaultAllowed: THIRD_PARTY_COOKIES_DEFAULT, site: _site,
                                        exception: _site === null ? null
                                          : self.safeFetchSiteSetting(_site, "third-party-cookies") } };
    }
    /* The one setting this surface carries: how much of the person's disk the frontier's stored documents may
       occupy. It is a knob on storage, never on work (an instance count or a throttle would be a bound): every
       recipe, counter and entry survives at every setting, including zero, and only whether a document is held
       or fetched back changes. The reply carries the value now in force, so a caller can read back what took. */
    if (msg.type === "AST_FRONTIER_SHARE") {
      DCHECK(msg.bytes === undefined ||
             (typeof msg.bytes === "number" && Number.isFinite(msg.bytes) && msg.bytes >= 0),
             "a frontier-share setting arrived that is not a byte count (`" + String(msg.bytes) + "`) — it is " +
             "compared against the stored document halves, and a value no comparison is true of is an " +
             "UNLIMITED store wearing the appearance of a configured one");
      let _persistError = null;
      if (typeof msg.bytes === "number" && Number.isFinite(msg.bytes) && msg.bytes >= 0) {
        _frontierShare = msg.bytes;
        _persistError = await prefWriteOutcome(frontierPrefPut("share", msg.bytes));
        /* The setting takes effect immediately: residency is an invariant of the store and this is one of the
           two things that can break it (the other is a write). */
        await frontierResidency();
      }
      /* The two awaits are named before the reply is built, so every entry of this seam's reply record has a
         binding as its receiver. `frontierIndex` writes none of `_frontierStats`, so hoisting it moves no
         number. */
      const _share = await frontierShare();
      const _index = await frontierIndex();
      return { success: true, result: { share: _share, defaultShare: frontierDefaultShare(),
                                        persistError: _persistError,
                                        docBytes: _frontierStats.docBytes, overShare: _frontierStats.overShare,
                                        shed: _frontierStats.shed, stranded: _frontierStats.stranded,
                                        rederived: _frontierStats.rederived,
                                        crashed: [..._index.values()].filter((r) => r.crashed).length,
                                        entries: _index.size } };
    }
    /* ─── The person's egress sentence, and the extension's one door onto it ─────────────────────────────
       Firing what a bundle reaches only past a forced gate is configurable per origin, default conservative and
       never inferred; this is how the shipped extension's person states it (`--explore <origin>` is the command
       line's equivalent). It decides nothing: what a widening means, what may be widened and what each request
       is answered with are lib/safe-fetch.js's, where SECURITY.md puts network policy.
       Authorization is the router's: `handlePopupMessage` gates on `sender.origin === EXTENSION_ORIGIN` before
       this. What this entry asserts is that a human initiated the act, which no principal can answer: the grade
       travels from the surface that knows (`PAGE_CONTEXT_USER_INITIATED`, lib/schema.js's constant), and an
       absent one refuses. `subject` is echoed with its refusal, from `safeFetchFiringRefusal`, the function
       the chokepoint refuses with, so the control shows what this origin's forced data requests are answered
       with before and after the change. */
    if (msg.type === "AST_EGRESS_POLICY") {
      CHECK(msg.initiator === PAGE_CONTEXT_USER_INITIATED,
            "the per-origin egress widening was reached with the initiator grade " +
            JSON.stringify(msg.initiator) + " — a widening is a PERSON'S SENTENCE and CLAUDE.md says it is " +
            "\"never inferred from a site looking like a test\", so the one thing this command may not do is " +
            "be issued by something that decided on its own. A caller that cannot state the grade never " +
            "answered the question, which is why an absent value takes this arm and not the permissive one");
      /* At most one command per message, checked by count rather than by enumerating pairs, so a fourth command
         needs no new rule. */
      DCHECK([msg.grant, msg.revoke, msg.permit].filter((c) => c !== undefined).length <= 1,
             "the egress command carried more than one of grant/revoke/permit in one message — they are " +
             "competing sentences about one table and the order they would be applied in is whichever this " +
             "entry happens to test first, which is a permission decided by the shape of an `if`");
      DCHECK(typeof msg.subject === "string",
             "the egress command carried no subject — it is the origin the surface is SHOWING the person, " +
             "and the refusal answered below is about it, so without one the control would report a policy " +
             "answer for nothing while the person read it as being about the page in front of them");
      /* A grant is refused with its reason, never asserted: `safeFetchWiden` aborts on an origin it cannot use,
         right for a caller in this project and wrong for a person viewing a sandboxed or `file://` page (an
         opaque origin is ordinary on the web, and a fatal here would give a page an abort switch on the trusted
         zone). The chokepoint's own predicate is asked first and its answer carried to the surface. */
      let refused = null;
      let changed = false;
      if (msg.grant !== undefined) {
        DCHECK(typeof msg.grant === "string",
               "the egress command was asked to grant " + JSON.stringify(msg.grant) + " — a widening names " +
               "an ORIGIN, and a non-string would reach the chokepoint's parse as a permission about nothing");
        refused = self.safeFetchWidenable(msg.grant);
        if (refused === null) { self.safeFetchWiden(msg.grant); changed = true; }
      } else if (msg.revoke !== undefined) {
        DCHECK(typeof msg.revoke === "string",
               "the egress command was asked to revoke " + JSON.stringify(msg.revoke) + " — a revocation " +
               "names an ORIGIN, and a non-string would silently remove nothing while reading as a " +
               "permission the person had just taken back");
        changed = self.safeFetchUnwiden(msg.revoke);
      } else if (msg.permit !== undefined) {
        /* One signal, one value, one origin per message: the row was declared by lib/safe-fetch.js and rendered by
           the surface, and whether it exists, whether the value is its own and what permitting it means are asked
           of the chokepoint. Not a batch, so the table a person reads back is the policy in force. */
        DCHECK(msg.permit !== null && typeof msg.permit === "object" &&
               typeof msg.permit.origin === "string" && typeof msg.permit.signal === "string" &&
               typeof msg.permit.value === "string" && typeof msg.permit.allow === "boolean",
               "the egress command carried a permission that is not an {origin, signal, value, allow} — a " +
               "field missing here would reach the chokepoint as `undefined`, which its own predicate " +
               "refuses, so the person would be told their row could not be permitted for a reason about " +
               "this message rather than about their app");
        /* Refused with its reason, for the grant's reason. The origin and the row are both asked, since a good
           origin with a retired signal would otherwise abort inside `safeFetchPermit`'s CHECK. */
        refused = self.safeFetchWidenable(msg.permit.origin);
        if (refused === null) refused = self.safeFetchSignalUsable(msg.permit.signal, msg.permit.value);
        if (refused === null) {
          self.safeFetchPermit(msg.permit.origin, msg.permit.signal, msg.permit.value, msg.permit.allow);
          changed = true;
        }
      }
      /* The store moves only where the table did, written from the table, so it never holds a permission the
         chokepoint refused. */
      const _persistError = changed ? await prefWriteOutcome(egressPolicyPersist()) : null;
      /* A changed policy is the host event a residue resting on refusals waits for (frontierRestHolds), and the
         Level-1 walk reads the policy only on a round, so a round is asked for. */
      if (changed) _hostKick();
      /* A subject the chokepoint could not widen is not probed: `safeFetchFiringRefusal` takes an absolute URL and
         throws on an unparseable one. `null` states "this origin cannot be the subject", rendered as the reason. */
      const _subjectUsable = self.safeFetchWidenable(msg.subject);
      /* The probe is the hardest case a person decides about: a forced (`provenance` and the record's reach),
         tool-composed (`actor: "tool"`; a page's own unpinned requests already fire by default), uncredentialed
         (as the learned-GET replay is) data request (`""`, never script-like) with `unstated` witness, at the
         origin's own root, the only address the surface has, so its `url-authority` row reads `unknown`. It
         claims nothing about how anything at this origin was actually reached or composed. */
      const _probe = { url: msg.subject + "/", destination: "", provenance: PROVENANCE_FORCED,
                       pinned: "unstated",
                       /* `public`: no document makes it, so its container would be a new one. */
                       doc: docRecord(PROVENANCE_FORCED, DRIVEN_NO, "unpinned", NAVIGABLE_NOTICE.sandbox.none,
                                      "public"),
                       actor: "tool", driven: "unstated",
                       credentialed: false, headers: null };
      return { success: true, result: {
        origins: self.safeFetchWidenedOrigins(),
        subject: msg.subject, subjectUsable: _subjectUsable,
        subjectRefusal: _subjectUsable === null ? self.safeFetchFiringRefusal(_probe) : null,
        /* What the person is deciding, row by row, from the policy's own derivation: `signals` is the registry
           with each value's state at this origin, `vector` what this probe's facts compute (which row the current
           refusal is about), `defaults` what fires without anyone saying so. All read from lib/safe-fetch.js. */
        signals: _subjectUsable === null ? self.safeFetchPermitted(msg.subject) : [],
        vector: _subjectUsable === null ? self.safeFetchSignalVector(_probe) : null,
        defaults: self.safeFetchDefaultArms(),
        legacyDropped: _egressLegacyDropped,
        refused, changed, persistError: _persistError } };
    }
    if (msg.type !== "AST_ANALYZE") return { success: false, error: "unknown type " + msg.type };
    /* Enqueue this document into the live host WFQ pool: its instance interleaves in slices with every other
       open document by value of information. The promise resolves when this engine finalizes (fully explored or
       evicted); the pool persists its residue to the cross-session frontier when `msg.persist` is set. */
    /* It carries the browser's name for the document, which admission uses to recognise a RESHIP re-delivery
       rather than seat a second engine. Every live document comes from _dispatchDocument, which reads the id off
       the browser-provided sender; a peer-announced child and a rehydrated recipe build their engine directly
       and never reach here. */
    DCHECK(!!msg.documentId,
           "an AST_ANALYZE for a live document carried no documentId — it is the name the pool answers " +
           "\"which instance holds this document?\" by, so without it a re-delivered document is seated twice");
    documentPolicyOpen(String(msg.documentId));
    /* And its agent cluster, both halves browser-stated, asserted where the facts are born: a missing group or
       origin would merge documents into one heap behind one principal while looking like the design working.
       `frameId` is asserted for presence, since 0 is the top frame. */
    DCHECK(msg.groupId != null && msg.groupId !== "",
           "an AST_ANALYZE for a live document named no browsing-context group — an instance is its " +
           "(group, origin) agent cluster, and _dispatchDocument carries the browser's sender.tab.id as the group");
    DCHECK(typeof msg.origin === "string" && msg.origin !== "",
           "an AST_ANALYZE for a live document named no principal — the cluster's origin half is the browser's " +
           "MessageSender.origin (opaque-unique via _senderOrigin), and an empty one collapses every document " +
           "of this tab into one heap; it is never derived from the address, which a sandboxed document's lies about");
    DCHECK(typeof msg.frameId === "number",
           "an AST_ANALYZE for a live document carried no frameId — the pool reads it to tell a sub-frame " +
           "(which its cluster's instance already runs) from the group's TOP document (which it does not), and " +
           "an absent one is indistinguishable from frame 0");
    const persist = !!msg.persist;
    /* ─── The seed's own navigation, performed here ──────────────────────────────────────────────────────
       The trusted zone hands this entry an address the ambient observer suggested, and the document is loaded
       here through navigationLoad, the same path as a child navigable's load; nothing arrives carrying bytes.
       The private-network principal is the browser's: `msg.sourceUrl` is the address the browser reported, so
       a suggested address cannot authorize itself into the person's intranet. The admitting zone has already
       established the two are same-origin, which is all HTML §7.2.5 The History interface lets `pushState`
       change. */
    DCHECK(typeof msg.seedUrl === "string" && msg.seedUrl !== "",
           "an AST_ANALYZE for a live document named no seed — a seed is an ADDRESS and it is the whole of " +
           "what an ambient observer contributes, so a dispatch without one is a document nothing can load");
    DCHECK(typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
           "an AST_ANALYZE for a live document carried no browser-stated address — it is the private-network " +
           "principal this navigation is classified against, and without it a suggested address would be " +
           "classified against itself");
    DCHECK(msg.pageHtml === undefined && msg.responseHeaders === undefined,
           "an AST_ANALYZE for a live document arrived carrying a document body or a header list — a seed is " +
           "an address and never bytes, so a producer that ships either has rebuilt the second, unpoliced " +
           "document-load transport this entry exists to be the only alternative to");
    /* It carries the person's session: this is one of the custom browser's own tabs and the person navigated
       here. The credentialed-read principal is `msg.origin` (MessageSender.origin, minted by `_browserFacts`),
       never `originOf(msg.sourceUrl)`. It is same-origin by construction (the admitting zone refuses a seed
       cross-origin to the browser-stated address); this second comparison is against a browser-stated origin,
       which is what makes a sandboxed document's load uncredentialed while its seed is admitted.
       Its provenance is `observed`, true here as nowhere else: solver/engine.h defines it as "a real load of
       this document makes exactly this request", and the address is the one the browser actually navigated to
       (`PerformanceNavigationTiming.name`, which `pushState` cannot forge). Stated by the zone that knows, never
       derived from the address. */
    /* `observed` for the issuing context too: the browser really navigated to the page this seed came from. */
    /* `actor: "tool"`: `observed` says the person's browser made this exact request; `actor` says who composes
       the request this zone is about to make, which is this tool, from an address an observer reported
       (safe-fetch.js's `_actorOf`: a request this tool composed at a person's direction is still `tool`).
       engine/trusted.mjs's command-line seed states the same word. */
    const loaded = await navigationLoad(msg.seedUrl, msg.sourceUrl, msg.origin,
                                        /* `none`: this seed can report an unavailability through
                                           `onNavigationOutcome` but not a decline, since offscreen-brain.js's
                                           `_PAGE_SOURCE_KINDS` (status/empty/network) all say a load was
                                           attempted.
                                           Named residual: a declined ambient seed is not covered. Next diff
                                           adds a fourth page-source kind through `_PAGE_SOURCE_KINDS`, its
                                           pairing asserts and the popup, then this word becomes `report`.
                                           Absence shows as this loader's `none` abort naming an ambient
                                           seed's address. It is unreachable while both destructive gates are
                                           scoped to `provenance !== "observed"` and the observed/observed
                                           arm fires at every address. */
                                        PROVENANCE_OBSERVED,
                                        docRecord(PROVENANCE_OBSERVED, DRIVEN_NO, "unpinned",
                                                  NAVIGABLE_NOTICE.sandbox.none,
                                                  self.safeFetchDocumentAddressSpace(msg.sourceUrl, null)),
                                        /*refusalArm*/"none",
                                        /*actor*/"tool", /*driven*/"unstated", /*pinned*/"unstated",
                                        DOCUMENT_REQUEST_UNSTATED, seedSpeaker(msg),
                                        /* a document the person's browser loaded states no zone flags */ false);
    bootSpanMark(String(msg.documentId), "loaded");
    const unavailable = seedUnavailable(loaded, originOf(msg.sourceUrl));
    /* The outcome is reported the instant the load settles, so a person opening the popup over an empty panel
       can tell "failed" from "nothing yet" while the analysis runs. */
    DCHECK(typeof self.onNavigationOutcome === "function",
           "the trusted zone has no onNavigationOutcome to record this navigation's outcome against — it is " +
           "the ONLY writer of a document's page-source record now that the load happens here, so without it " +
           "a page whose seed could not be loaded is a silence in the only zone that renders one");
    if (unavailable !== null) {
      documentPolicySettle(String(msg.documentId), null);
      self.onNavigationOutcome(msg.documentId, Object.assign({ state: "unavailable" }, unavailable));
      /* No engine runs, so there is no result document, the one analysis that legitimately has none. The reason
         is on the page-source record above. */
      return { success: true, result: linesToAnalysis([], msg, "nothing-to-run", null) };
    }
    self.onNavigationOutcome(msg.documentId, { state: "delivered" });
    /* The load's Set-Cookie fields go to the jars their URLs' origins key in this document's group. */
    await zoneRouteSetCookies(msg.groupId, loaded.setCookies);
    /* The document is at the address the response came from: HTML §7.4.5 determines the Document's URL, and so
       its origin, its API base URL (HTML §8.1.3.2 Environment settings objects) and the frontier key, over the
       response's URL, as `fetchedDocument` does for a child navigable. */
    msg.sourceUrl = loaded.url;
    msg.responseHeaders = loaded.headers;
    msg.credentialed = loaded.credentialed;
    /* The word the load was performed under, written onto the record (this run persists, so its residue is
       re-fetched later from what this states); the same constant as the load, so the two cannot disagree. */
    msg.provenance = PROVENANCE_OBSERVED;
    /* The policy container of a document the person's browser loaded is the browser's; this zone knows it
       through the response's address (Local Network Access §3.5).
       Named residual: a browser-reported local-scheme frame (match_origin_as_fallback runs content.js in a
       data:, blob: or about: frame) is given a new container, initially public, where HTML §7.1.7 clones its
       navigation initiator's. No browser-stated fact names the initiator: webNavigation's parentDocumentId is
       the parent, which a sibling's navigation is not, and the frame's sender.origin does not fix the
       initiator's container (FileAPI §8.3.2 lets a top-level navigation load another origin's blob: URL), so
       either would widen. Next diff reads the initiator from a browser source that states it, the DevTools
       Protocol under the `debugger` permission being the unmeasured candidate. Absence shows as
       `blocked-local-network:<space>-from-public` on a request from a local-scheme frame of a loopback or
       local page. */
    msg.addressSpace = self.safeFetchDocumentAddressSpace(loaded.url, null);
    msg.arrival = docRecordArrival(docRecord(PROVENANCE_OBSERVED, DRIVEN_NO, "unpinned",
                                             NAVIGABLE_NOTICE.sandbox.none, msg.addressSpace));
    /* A document the person's browser loaded: the zone states no flags for it, since its sandbox is the
       browser's own to enforce and the engine computes what its response's CSP adds. */
    msg.sandbox = NAVIGABLE_NOTICE.sandbox.none;
    /* The waiter carries both settlers, so a Clear can reject a document never seated with "cleared", which
       _dispatchDocument reads to abandon its tail without recording a page failure. No `code` is carried: no
       producer reaching this entry writes one. */
    /* The caller is answered at the instance's finalize, after any install has settled the referrer policy entry;
       a document no instance installed (cleared, failed boot) settles it here. */
    let result;
    await level1AskSites([msg.sourceUrl]);
    bootSpanMark(String(msg.documentId), "queued");
    try { result = await new Promise((resolve, reject) => { _waiting.push({ html: loaded.bytes, msg, persist, resolve, reject }); _hostKick(); }); }
    finally { documentPolicySettle(String(msg.documentId), null); }
    return { success: true, result };   // fetchCallSites is already deduped by the engine
  } catch (e) {
    /* A dispatch that threw installed nothing, so a referrer policy entry it opened is settled before anything
       else, or a sub-frame seed waiting on it would wait for ever. */
    if (msg && msg.type === "AST_ANALYZE" && _documentReferrerPolicies.has(String(msg.documentId)))
      documentPolicySettle(String(msg.documentId), null);
    /* An invariant abort is not an analysis failure: this catch reports a failed dispatch (`_astError`), right
       for a page that could not be analysed and wrong for a broken contract, which is rethrown. */
    RETHROW_FATAL(e);
    return { success: false, error: String(e && e.message || e), stack: e && e.stack };
  }
};

console.debug("[ast-worker v2] bridge ready (self.astDispatch + self.rendererPoolProbe installed)");

/* ─── The door engine/level1.mjs drives the Level-1 order through ──────────────────────────────────────
   Node only: `module` is undefined in ast-worker.html, so this ships nothing. The Level-1 order is composed
   only in this zone (no engine sees another), so a driver here is its only possible reader. What is exported
   is the order, never the state: `_bestCandidate` takes its whole population as an argument and
   `_candPopulation` composes it from this module's privates, so a driver hands its own population. The
   declarations (`CAND_KINDS`, `CAND_SPREAD`, `_candCensus`) travel with it because the census's contract is
   stated in them, and a driver's own copy would go stale silently. */
if (typeof module !== "undefined" && module.exports)
  module.exports = { hostSchedule, engineKey, engineRecordFacts, level1Index, level1KlUcb, level1At, level1Sum, level1Credit,
                     level1Rate, level1Tree, level1Key, level1Ahead,
                     level1SiteOf, LEVEL1_UNSERVED, LEVEL1_DOOR,
                     _bestCandidate, _candPopulation, _level1Record, _candCensus, CAND_KINDS, CAND_SPREAD,
                     documentIdentityOf, _frontierMembers, programDigest, egressPolicyNow,
                     zoneRouteSetCookies, zoneAnswerSetCookies, zoneOpenJar, zoneReclaimHeldCookies,
                     zoneTabClosed, engineDeliverDocument, clusterKeyOf };
