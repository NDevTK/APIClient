/* THE SOLVER'S CORRECTNESS GATE — the one whose failure means the SOLVER's semantics regressed.
 *
 * WHY IT EXISTS. Neither existing gate links or judges this half, and that is checkable rather than arguable:
 *   - engine/test262.mjs builds run-test262 from `["quickjs.c","libregexp.c","libunicode.c","dtoa.c",
 *     "quickjs-libc.c","run-test262.c"]`. Nothing under engine/host is in that binary, so a green 43520 says
 *     exactly nothing about decide.c, cow.c, concolic.c, flow.c, engine.c or cold.c. It judges the FORK — the
 *     trampoline, the step machines, the preempt seam — against ECMA-262's own oracle, which is what it is for.
 *   - engine/wpt.mjs DOES link the solver (it walks host/solver and host/browser), so a solver DCHECK can fire
 *     during it — but its oracle is testharness.js, which asserts WHAT A BROWSER ANSWERS. Not one WPT file
 *     mentions a decision vector, a COW delta or an emitted endpoint, so it can only catch a solver bug that
 *     also breaks a spec answer. That is a real signal and it is not this one.
 *   - engine/host/test_forced.c is a FIXTURE: one hardcoded document, ~70 hand-written `strstr` probes, and it
 *     is the only thing that judges the solver at all. It tests what its author already thought of (the exact
 *     sentence engine/wpt.mjs's own header uses about the IDL audit), and it cannot see a change that is
 *     WRONG-BUT-CONSISTENT: invert the replay/refinement order in decide_arm, delete the fork's pre-record of
 *     the sibling's arm, remove the flat birth vector — all of which happened in one session — and every probe
 *     still passes, because every probe reads ONE run of ONE schedule and asks whether a string is in it.
 *
 * SO WHAT IS THE ORACLE. Not a declared expected emission per fixture: a set of endpoints and findings written
 * down beside a document is a CHANGE DETECTOR, and CLAUDE.md forbids exactly that shape twice over ("Use design
 * review, not a test metric as a target — a smaller number is not automatically worse"; "Delete a regression
 * test after use — it prevents better designs"). A solver that learns MORE would fail it, which makes the gate
 * an argument against the project's own goal. Not a set of run invariants either: those belong at their ORIGIN
 * as DCHECKs and are already there (`g_c <= dec_total()`, `cursor == dec_total()`, the chain's `below` offsets),
 * and a gate that restates them from outside is a second, weaker copy of an assert that already fires louder.
 *
 * THE ORACLE IS THE DOCUMENT ITSELF, ASKED TWICE UNDER DIFFERENT SCHEDULES. CLAUDE.md states the property
 * directly and in three places, so it is spec here in the same sense ECMA-262 is spec to test262:
 *
 *     §scheduler   "A flow's OWN await chain resumes strictly in program order; only WHICH flow runs next is
 *                   value-reranked."
 *     §scheduler   THE RAZOR: "if a resume is not byte-identical — drops, starves, skips, reorders, or forgets
 *                   ANY flow — it is a CAP, banned; a yield you cannot prove is lossless is a cap."
 *     §Time-travel "the same execution continued, byte-identical, at any loop depth or await."
 *
 * Together those say: for a document whose frontier DRAINS, the set of findings is a function of the DOCUMENT
 * ALONE. The schedule decides the ORDER work happens in and nothing else. So this gate runs each corpus
 * document under several schedules and requires the finding SETS to be equal. That is self-validating in
 * test262's sense — the answer is carried by the run itself, not by a table someone maintains — and it is the
 * one oracle a solver improvement cannot break: nothing is written down for a better solver to contradict. A
 * solver that finds twice as much passes unchanged. It fails only when ONE BUILD DISAGREES WITH ITSELF.
 *
 * WHAT A FAILURE MEANS, precisely: some flow was dropped, starved, skipped, reordered or forgotten by one of
 * the schedules — which is the definition of a cap — or a swapped-in flow read state belonging to another one.
 * Both are solver-semantics regressions and neither has any other detector in this tree.
 *
 * WHAT IT DOES NOT COMPARE, and each name is here because it is a COST rather than a finding: `_switches`,
 * `_flows`, `_candidates`, `_jobsQueued`, `_jobsRun`, `_unitsDone`, `_worldSegmentsHeld`, `_worldSegmentsMade`,
 * `_worldSegmentsForked`, `_park`, the four numbers of the @S arrival census (`_sourceReads`, `_sinkReached`,
 * `_sinkTainted`, `_sinkSuppressed`), the routed-delivery pair and the four ends of the task it queues
 * (`_routedDelivered`, `_routedRefused`, `_routedTasksFired`, `_routedTasksTargetOrigin`,
 * `_routedTasksTargetGone`, `_routedTasksThrew`), the orphan census (`_orphansDriven`, `_orphansAsked`), the
 * four subsystem censuses (`_cold`, `_heap`, `_swap`, `_forkAt` — dropped as READINGS OF AN INSTANT rather
 * than as costs; see the row itself, and note that `_wfq` is deliberately NOT dropped because the terminal
 * document's frontier is empty under every schedule), and a parked search's `tried` and `turns`. `_switches` exists precisely BECAUSE it differs between an
 * interleaving scheduler and a FIFO one (result.c says so), so comparing it would fail every schedule by
 * construction. THE ARRIVAL
 * CENSUS IS ARGUED IN ON THAT SAME GROUND AND NOT WAIVED: each of its four is a count of EVENTS, and the
 * number of events scales with how many flows and how many candidate re-runs happened — quantities this list
 * already accepts differ between schedules (`_flows`, `_candidates`). A sink executed by six flows and by
 * seven is the same DOCUMENT, and the census is about coverage rather than about what was found. What it
 * costs to drop them is stated plainly: this gate cannot then catch the census itself regressing, which is
 * correct for a cost surface and would not be for a finding.
 * THE SIX CROSS-INSTANCE COUNTERS ARE ARGUED IN ONE AT A TIME, because they are two different kinds of number
 * and no single sentence covers both. `_routedDelivered` and `_routedRefused` are counted once per (routed
 * record, TIMELINE it was offered to) — solver/engine.h: a routed record is attached to EVERY LIVE FLOW of the
 * receiving document, each of which either admits it or consumes it as belonging to the other side of a sender
 * branch. So their SUM is the number of flows standing when the host routed the record, which is a fact about
 * how far exploration had got and not about the document; and that header names the schedule-dependence in
 * this gate's own words, saying of a host that compares its routed count against a page's handler invocations
 * that "it passes while they are starved and fails the moment they run, which is the schedule-dependent answer
 * §Testing's differential exists to catch". Holding the SUM invariant instead is not a way out: the sum is
 * that same flow count. The four ends of HTML §9.3.3 "Posting messages" step 8's task inherit it, because each
 * is counted once per queued task per arm that reaches it — and engine.h states the conservation law as
 * SUM >= `delivered` rather than `==` precisely because a fork landing BETWEEN the enqueue and the run makes
 * two ends out of one queued task, and whether a fork lands in that window is the schedule's choice and
 * nothing else's. TWO OF THE FOUR DIFFER FOR A SECOND REASON OF THEIR OWN, worth stating because it is not the
 * first: `_routedTasksTargetOrigin` is step 8.1 checked INSIDE the task, so the origin it compares is the one
 * the target has THEN and a navigation may land between the post and the delivery; and `_routedTasksTargetGone`
 * is §7.5.10 "Destroying documents"'s destroy-a-document step 7 ("remove any tasks whose document is document
 * from any task queue (without running those tasks)") reaching a task before it runs. Both are outcomes of an
 * ORDER between two work items, which is the one thing a schedule is free to choose. None of the six is a
 * finding: what a routed message CAUSES in this document arrives in fetchCallSites / securitySinks /
 * pageErrors by executing like anything else, and these six count the transport that carried it.
 * AND THEY ARE STRUCTURALLY ZERO IN THIS GATE, said out loud rather than left looking like coverage. A routed
 * record reaches an engine only through main.c's `qjs_route`; this driver never calls it, and window_message.c
 * and flow.c both gate the four ends on the record being ROUTED, so a local post reaches the same ends and is
 * not counted. A corpus document that would need a peer is refused above by name. So this row is a
 * classification made for the driver that DOES route — engine/route.mjs, which reads all six and asserts each
 * rather than defaulting it — and not an exclusion this gate is exercising. Everything
 * else is compared BY DEFAULT — a field added to the result document is a field this gate holds invariant
 * until someone argues it into the list above, which is the direction that fails loud rather than quietly.
 *
 * Usage:  node engine/solvergate.mjs [document-name]
 */
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join, basename } from "node:path";
import { fileURLToPath } from "node:url";
import { loadavg, cpus } from "node:os";
import { gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";
import { childCpuSeconds, childCpuDelta, cpuText } from "./gate_cpu.mjs";
/* THE SHIPPED ARTIFACT, and deliberately not a runner of this gate's own. engine/wpt.mjs builds its own native
   runner because it runs 800 files and an eight-minute wasm link per iteration is a gate nobody runs; this one
   runs a handful of documents through the ENTRY THE EXTENSION LOADS, which costs one import and makes the gate
   a second driver of the production ABI besides engine/route.mjs. §Testing: the shipped entry is the one that
   rots, and a gate that exercises it is worth more than one that exercises a fixture beside it.
   AND THE PATH IS NOT SPELLED HERE, because this gate and engine/route.mjs are the two drivers that `ccall`
   that entry RAW — no transport, nothing checking them at either end — and both check their operands against
   what THAT artifact declares. One fact, one place: engine/renderer_abi.mjs, which also holds the ONE walk of
   `content.mojom.Renderer.Init`'s own parameter list that both of them place through. This gate used to keep
   a hand-aligned array of fourteen values in declaration order instead, and when HTML §3.1.3's ancestor
   origins statement became the fifteenth it went short: emscripten zero-fills the too-FEW direction in
   silence, so `navigable_root_ancestor_origins` aborted EVERY document under EVERY schedule and the solver's
   only oracle answered nothing while reading as one document's bug. */
import { GLUE_PATH as WASM, abiOperands } from "./renderer_abi.mjs";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const SELF = fileURLToPath(import.meta.url);
const CORPUS = join(ENGINE, "tests", "solver");

/* THE SCHEDULES, AND EACH ONE IS A TRIPLE RATHER THAN A NAME. Every knob is one the production host already
   has and uses, so none of them is a test hook grown into the engine for this gate's benefit. A schedule
   declares ALL THREE of its policies — when the ENGINE hands the thread back, when the HOST answers what it is
   owed, and WHAT THE HOST ASKS OF THE ENGINE AT THE BOUNDARY — because those are three independent decisions
   and this file used to let the second one be INHERITED from the first. That inheritance is what made the
   gate's strongest-sounding axis its weakest, so it is worth the space to say exactly how, in the terms of the
   two mechanisms rather than in the terms of the names:
 *
 * WHAT `_switches` COUNTS. solver/engine.c increments it at exactly ONE statement, inside engine_sched_slice's
 * dispatch loop: `if (best != cur) { flow_switch_out; flow_switch_in; solve_flow_begin; g_switches++; }`. It is
 * a FLOW context switch — the COW delta swap, the decide_suspend/decide_resume pair and the concolic pin swap —
 * and it happens only when flow_next_to_run returns something other than the flow already holding the thread.
 *
 * WHAT THE YIELD FLOOR CAUSES. qjs_set_yield_floor writes engine.c's `g_yield_floor`, which is read at the
 * BOTTOM of that same loop, AFTER the step: `if (cur && flow_weight(cur) < g_yield_floor) { g_sess_cur = cur;
 * return ENGINE_STEP_YIELD; }`. It ends the SLICE. The flow holding the thread is stored in `g_sess_cur` and
 * picked straight back up by the next slice — engine.c asserts precisely that where it does it ("the flow this
 * quantum resumes is no longer in the frontier" DCHECKs `flow_is_member(cur)`) — and flow_pick then takes that
 * same flow as its SEED, which it displaces only for a STRICTLY greater weight. So the floor moves nothing the
 * switch count is a function of: it cannot change which flow the pick returns, and therefore CANNOT BY
 * CONSTRUCTION change `_switches`. It is a HOST-yield knob, which is what main.c's own contract calls it — "the
 * runner-up ENGINE's weight, so the running flow yields the moment it is outranked ACROSS documents" — and this
 * file used to describe it as the thing that makes the engine "context-switch maximally… thousands of times
 * instead of a handful". That sentence was false about the mechanism and measurably false about the run.
 *
 * WHAT ACTUALLY MOVED, MEASURED (flag_fork.html, 2 flows, one build, four cells):
 *     floor -inf, reply paid at the reporting boundary   3 host turns   13 switches
 *     floor +inf, reply paid at the reporting boundary  25 host turns    2 switches
 *     floor +inf, reply paid where the engine is stuck  28 host turns   13 switches
 *     floor -inf, reply paid where the engine is stuck   3 host turns   13 switches
 * The floor is neutral across the pairs that hold the reply policy fixed, exactly as the mechanism above says.
 * What collapsed the run to 2 was the REPLY POLICY — and the floor changed it as a SIDE EFFECT, because a host
 * that pays at every boundary pays sooner when the boundaries are eight times closer together. Two switches is
 * NULL->A and then A->B after A finished: the frontier ran STRICTLY SERIALLY, which is the FIFO order this
 * gate's own counter exists to distinguish from an interleaving one (solver/engine.c says so where it counts).
 *
 * WHY THE REPLY POLICY IS THE ONLY LEVER A HOST HAS ON INTERLEAVING. flow_pick lets the incumbent keep the
 * thread unless something strictly outranks it, with ONE exception it writes into the seed test itself:
 * `!(runnable_only && flow_host_owed(seed))`. A host-owed mark is the one input to that pick a HOST can move,
 * and it moves it by choosing when to pay. Answer a flow's reply in the turn it is reported and the mark is
 * cleared before the pick is next made, so that flow never leaves the thread and no sibling ever takes it.
 * Hold it and the sibling runs. That is the whole of the mechanism, and it is why the reply policy is now a
 * declared field instead of a consequence of how often the engine happens to return.
 *
 * WHAT WOULD BE NEEDED TO FORCE MORE SWITCHING THAN THIS, named precisely so nobody reads the axis as stronger
 * than it is: a LEVEL-2 floor — the same comparison flow_pick already makes, but with the incumbent given no
 * defence, i.e. the pick asked as flow_next_to_run(NULL) so an EQUAL sibling displaces the running flow instead
 * of only a strictly better one. No such knob exists, the engine's own VALUE yield is defined on strict
 * outranking (§scheduler: "suspend the moment a parked flow OUTRANKS"), and this gate must not grow one: a
 * schedule knob that no production host has is the test hook the first line of this comment refuses.
 *
 *   `direct`   — the reference, and it is production's own pair: default yield floor (-inf: run on) with the
 *                whole owed list answered at the boundary the engine reports it on (extension/bridge.js pays at
 *                EVERY slice boundary). Under this floor the engine returns only where it can make no further
 *                progress, so on a document that fits inside one cooperative quantum the reference's boundary
 *                IS the stuck one — which is why the reference has always been the most-interleaved schedule in
 *                the set, and why nobody noticed that the policy and the moment had come apart everywhere else.
 *   `preempt`  — qjs_set_yield_floor(+Infinity): the host's Level-1 VALUE yield with a rival this engine can
 *                never outrank, so the slice ends after EVERY unit of work. What that exercises is the SLICE
 *                BOUNDARY — §scheduler's razor, "the next step must resume the SAME top flow on the
 *                byte-identical frontier", asked once per unit of work instead of once per document — plus the
 *                quantum_begin/quantum_end bracket and the g_sess_cur carry at maximum frequency, with the
 *                frontier INTERLEAVED while that happens. It states "stuck" rather than inheriting the
 *                reference's word, and the two are not the same policy under this floor even though they are
 *                the same words: a boundary that arrives once per unit of work is not the boundary the
 *                reference pays at, so the schedule that maximises boundaries has to name its reply moment or
 *                it silently varies the reply LATENCY instead of the floor — which is what it did, and the
 *                order it delivered was `eager`'s.
 *   `eager`    — the same floor with the reply answered in the turn it is REPORTED: zero reply latency, which
 *                is a real host answer (a cache hit, a body already in memory) and produces the strictly serial
 *                FIFO frontier. It is kept as its own schedule rather than deleted, because an interleaved
 *                order and a FIFO order over one document is exactly the comparison a differential wants, and
 *                because this is the behaviour the name `preempt` used to carry — parked under a name that
 *                describes it instead of under one it contradicted.
 *   `lastreply`— replies answered ONE PER TURN and in reverse arrival order. This does not change when a flow
 *                yields, it changes WHICH flow becomes runnable first — so the fork tree is built in a
 *                different order. §Learning-from-replies says the reply is the same reply whenever it lands.
 *   `park`     — the frontier is PARKED at the first step boundary (qjs_request_park), the residue is carried
 *                through the result document exactly as extension/bridge.js carries it to IndexedDB, and a
 *                SECOND INSTANCE resumes from it. §Time-travel-resume's whole claim, end to end, in one
 *                process. See the child's own comment for why the park is taken before the first pick and what
 *                the deeper park still needs.
 *   `stream`   — the reference's own (floor, reply) pair with `qjs_emit_partial` called at EVERY boundary: the
 *                CONTROLLED TWIN of `direct`, differing from it in exactly one thing, so a mismatch row against
 *                the reference is attributable to that one call and to nothing else. What it tests is the
 *                contract main.c writes over that entry in as many words — "It READS: no flow is touched,
 *                nothing is drained, and the frontier the next step resumes is the one this was called on" —
 *                which is a SCHEDULE-INVARIANCE claim and had no reader anywhere in this tree. See the entry's
 *                own block below for why an entry the extension calls on a cadence for the whole of every long
 *                analysis had never once been driven by a gate.
 *   `eagerstream`— `eager`'s own (+inf, "reported") pair with that same call at every boundary: the CONTROLLED
 *                TWIN of `eager`, and the other end of the axis `stream` opens. The two streaming rows differ
 *                from each other in the FLOOR alone and each differs from its non-streaming pair in the third
 *                field alone, so a mismatch on either names one call or one floor and never a choice between
 *                them. It exists because the floor decides HOW OFTEN the entry is asked: at -inf the engine
 *                returns only where it is stuck, so `stream` asks it a handful of times per document, while at
 *                +inf the slice ends after every unit of work. A perturbation that needs more than a handful of
 *                reads to accumulate is visible to this row and to nothing else in the set.
 * `direct` is the reference because it is what the extension does when no other document is competing.
 *
 * AND THE REPLY POLICIES ARE THREE, EACH NAMED FOR WHAT IT MODELS RATHER THAN FOR WHEN IT FIRES:
 *   "reported" — the whole owed list, in the turn the engine reports it. Zero latency.
 *   "stuck"    — the whole owed list, at the boundary where the engine says it can make no progress at all
 *                (ENGINE_STEP_STALLED). This is LATENCY, not laziness, and it is the honest single-engine model
 *                of the production round trip: extension/bridge.js takes the engine out of the hot set
 *                (`state = "fetching"`) and the reply lands from safeFetch in some LATER turn, never in the one
 *                that reported it, while other work runs meanwhile. This gate has one engine, so the work that
 *                runs meanwhile is this document's other flows — which is the point. It is NOT bridge.js's
 *                "pay only at a stall", the policy engine.c argues against: that argument is about withholding
 *                a reply that has ALREADY ARRIVED, and these replies are minted here and have never been in
 *                flight, so this driver has to choose an arrival moment and a network does not deliver into the
 *                turn that asked.
 *   "last"     — one entry per turn, tail first. `lastreply`'s whole content.
 *
 * AND THE THIRD POLICY IS WHAT THE HOST ASKS OF THE ENGINE AT THE BOUNDARY, WHICH IS THE ONE THING PRODUCTION
 * DOES THAT THIS GATE'S OWN REFERENCE NEVER DID. `direct` is described three lines up as "production's own
 * pair", and it was a pair while production makes THREE decisions per round: extension/bridge.js's
 * `streamPartial` calls `qjs_emit_partial` on a `PARTIAL_MS` cadence for the whole of any analysis that
 * outlives it, "WITHOUT waiting for a finalize that an unbounded engine never reaches" — its words — so on
 * every real page the engine is asked for a mid-run document between steps, and the reference that claims to
 * model it never asked once. It is not a knob invented here (§the first line of this comment): it is a shipped
 * ABI entry with a production caller and, until this schedule, no driver caller at all — `engine/route.mjs`
 * does not call it and neither did this file, so the entry's contract was carried entirely by main.c's own
 * prose. That is the §Testing shape exactly: the shipped entry is the one that rots, and this one composes the
 * finding document the product's incremental merge is built out of.
 *   false — the host asks nothing between steps. Every schedule but `stream` and `eagerstream`, which are named
 *           rather than counted by position: this line read "every schedule above" while exactly one row
 *           declared `true`, and a second one landing below it would have made the sentence quietly false.
 *   true  — `qjs_emit_partial` at EVERY boundary the engine reports, plus once more after the frontier answers
 *           DONE. The last of those is compared against `qjs_result`'s document (see the parent's row): both
 *           are `result_json(g_ctx)` with no step between them, so any difference is one of the two composers
 *           or this driver's capture of the print sink. `qjs_emit_partial` is named in exactly five files —
 *           engine/build.mjs's QJS_ABI list, main.c, qjs_abi.h, solver/result.h and extension/bridge.js — so
 *           the streamed @RESULT document had one producer, one production consumer, and no driver reading it
 *           at all. Read at the revision this landed against; grep it again before repeating it. */
const POLICY = new Map([
  ["direct",    { floor: -Infinity, reply: "reported", partial: false }],
  ["preempt",   { floor:  Infinity, reply: "stuck",    partial: false }],
  ["eager",     { floor:  Infinity, reply: "reported", partial: false }],
  ["lastreply", { floor: -Infinity, reply: "last",     partial: false }],
  ["park",      { floor: -Infinity, reply: "reported", partial: false }],
  /* THE CONTROLLED TWIN OF THE REFERENCE — the reference's own floor and reply, so the ONLY difference is the
     third field and a mismatch has one candidate cause instead of two. IT IS DECLARED ON EVERY ROW ABOVE AND
     NOT DEFAULTED HERE, which is this file's own lesson about the reply policy repeated rather than re-learned:
     a field one schedule states and the others inherit is how the second decision came to be read off the
     schedule's NAME.
     AT FLOOR -inf THE ENGINE RETURNS ONLY WHERE IT CAN MAKE NO FURTHER PROGRESS, so this row asks the entry a
     HANDFUL of times per document and cannot accumulate a perturbation that needs more calls than that. The row
     below is the other end of that axis and is why this one is no longer alone. */
  ["stream",    { floor: -Infinity, reply: "reported", partial: true  }],
  /* THE CONTROLLED TWIN OF `eager` — its own (+inf, "reported") pair with the boundary question turned on, so
     this row differs from `eager` in exactly the third field and from `stream` in exactly the first. That is
     what makes a mismatch attributable: a row against `eager` accuses `qjs_emit_partial`, a row against
     `stream` accuses the FLOOR, and neither has two candidate causes. It is the second half of one axis, not a
     seventh name — at -inf the engine hands the thread back only where it is stuck, so `stream` asks the entry
     a handful of times, while at +inf the slice ends after every unit of work and the same entry is asked once
     per unit for the whole run.
     WHY BOTH ROWS AND NOT THE +inf ONE ALONE: main.c's contract over that entry is that it READS — "no flow is
     touched, nothing is drained, and the frontier the next step resumes is the one this was called on" — and a
     read performed a handful of times and a read performed at every boundary are different amounts of exposure
     to the same claim, while only the pair holds the floor as a controlled variable. Deleting `stream` would
     buy one child per document and lose the single-variable attribution that makes either row diagnostic.
     WHAT IT COSTS, stated because it is a real one: one more child process per document on every run of this
     gate, which is the whole-corpus cost this row has to be worth.
     MEASURED WHERE IT LANDED AND NOT PREDICTED, WITH ITS RUN COUNT AND ITS SPREAD — flag_fork.html against the
     build stamped f6cbdd9b, on a 4-core box at load 4.4-7.4. That quantum is WALL-denominated on emscripten
     (see the block beside REFERENCE), so this is a count the BOX moves and a single sample of it would state
     nothing: `stream` took 3-8 snapshots over SEVEN runs (5/8/8/7 as a child, 6/4/3 under the parent) and this
     row took 24-26 over SIX (24/25/25/26 as a child, 25/24 under the parent). What licenses the sentence above
     about the two ends of the axis is that those two RANGES DO NOT OVERLAP — 3-8 against 24-26 — so the
     separation is larger than the noise that was actually measured rather than larger than a noise nobody
     looked at.
     AND THE SPREAD IS THE FINDING, NOT A DISCLAIMER ON IT. A prediction stood here before those runs and gave
     each end of this axis a SINGLE number, "~3" against "~25". The +inf end holds. The -inf end has no single
     value to hold: its measured range is 3-8 over seven runs, so "~3" is not so much wrong as UNFALSIFIABLE AS
     STATED — it names the bottom of a range it does not admit exists, and any one run can be quoted to confirm
     it or to refute it. That is the shape to avoid here specifically, because the quantity is wall-denominated
     and this box moves it: a point estimate of a noisy count reads as a measurement and is a draw. Every number
     in this block therefore carries its run count and its range, and the only claim made from them is the one
     two non-overlapping ranges support.
     THE NUMBERS ARE A SPREAD AND NEVER A THRESHOLD — nothing in this file compares them, `midrun` is printed on
     `_switches`' own ground, and a row that started asserting one would be a metric used as a target. */
  ["eagerstream", { floor: Infinity, reply: "reported", partial: true  }],
]);
const SCHEDULES = [...POLICY.keys()];
const REFERENCE = "direct";
/* THE PAIR PRINTED BESIDE EVERY COST, AND IT IS NOW A TRIPLE — written once because it was written twice, and
   a second speller of a schedule's declaration is how a row comes to be labelled with a policy it did not run
   under. The header's whole argument is that a reader who cannot see WHICH pair produced a number reads a
   column of agreements as more pressure than it is; a third decision the label omitted would be that same
   defect with the field this diff added. */
const policyLabel = (s) => {
  const p = POLICY.get(s);
  return `${s}(${p.floor === Infinity ? "+inf" : "-inf"}/${p.reply}${p.partial ? "/stream" : ""})`;
};

/* THE REFERENCE IS ASKED TWICE, AND THAT IS NOT A FIFTH SCHEDULE. Every row this gate prints is a difference
   between two SAMPLES, and nothing anywhere established that two samples of ONE schedule agree — so the gate's
   own premise sat underneath every one of its results, unmeasured. §Testing states that premise as the stronger
   of two claims: "the finding set is a function of the DOCUMENT ALONE". "The same across schedules" is strictly
   weaker. If two runs of the reference under the reference's own policy can differ, then every MISMATCH row is
   two draws of a noisy process reported as a schedule effect, and a real cross-schedule cap is indistinguishable
   from that noise. Run-to-run determinism was ASSUMED here, which is the same shape as every other property in
   this tree that read as established because it had never been measured.

   AND THE ENGINE DOES NOT MAKE IT TRUE BY CONSTRUCTION, which is why this costs its child rather than being a
   tautology. The WFQ's aging term is denominated in CPU ACTUALLY CONSUMED, and three mechanisms carry that
   quantity into what runs next:
     - solver/engine.c charges `flow_age_running(quantum_thread_us() - t0)` and PICKS on the next statement. Its
       own DCHECK there REQUIRES the charge to move rank — "a flow consumed a whole COOPERATIVE QUANTUM of the
       thread and its rank did not move" — so a rank change on consumed CPU is the design, not an accident.
     - solver/flow.c reads it: `flow_silence_notch(f) = (f->cpu + acct_family_us(f)) / FLOW_SERVICE_US`, an
       integer division of real microseconds by ONE cooperative quantum, subtracted from the weight at
       FLOW_AGE_QUANTUM a notch. flow_next_to_run seeds with the flow already holding the thread and displaces it
       only on a STRICTLY greater weight, so which side of a 12 ms boundary a charge lands on decides whether
       there is a context switch at all.
     - preempt_hook's cooperative clause is `return quantum_expired();` and the scheduler loop returns to the
       host on that same question, so WHERE a flow parks and WHEN this driver next gets to answer an owed reply
       are both decided by consumed CPU. That second one moves the very axis `lastreply` moves deliberately.
   AND ON THE HOST THE EXTENSION ACTUALLY SHIPS THAT QUANTITY IS NOT EVEN CPU: quantum.c's emscripten branch
   answers quantum_thread_us() from CLOCK_MONOTONIC because emscripten answers every WASI clock from
   emscripten_get_now(), and engine.c states the consequence at the charge — a descheduled step is billed for
   time it did not burn, so a flow is demoted a notch early and a sibling runs sooner. The schedule this gate
   measures is therefore a function of the BOX as well as of the document.

   SO A FAILURE HERE IS NOT EXPLAINED BY "CPU VARIES", and that is the whole reason the check is worth its run.
   CPU variation is LICENSED to reorder — §scheduler: "only WHICH flow runs next is value-reranked" — and
   FORBIDDEN to change what a DRAINING frontier emits. A differing SET means the order reached the outcome,
   which is a flow dropped, starved, skipped, reordered or forgotten (§scheduler's razor) measured with no
   second schedule involved at all. What is compared is the SET, never a sequence: the same canonical sets every
   other row compares.

   WHY IT IS NOT IN SCHEDULES. Each entry there is a distinct (floor, reply) PAIR that moves a distinct
   mechanism, and a duplicate of the reference moves none: putting it in that list would make the list untrue
   about itself, and would route a repeat failure through the MISMATCH message, which says "one of these two
   schedules dropped…" and would be a wrong diagnosis stated confidently. Two failures that read alike is the
   defect this file argues about @S candidates, performed by its own reporter. The verdicts are kept apart.

   WHY THE REFERENCE AND NOT ANOTHER. Every other comparison is made AGAINST it. A noisy `preempt` costs one
   row; a noisy `direct` costs the whole table, and the rows then have to say so — which is what refNoisy does.

   WHY LAST. The box is in a different state after the other children than before them, so the second sample's
   schedule has the best chance this gate can give it of differing from the first's. It costs nothing to put it
   there, and the switch counts printed beside the verdict are what say whether it worked: `held` beside two
   IDENTICAL switch counts is a weak sample — the two runs may have executed identically — while `held` beside
   13sw and 2sw is the invariant surviving a frontier that genuinely interleaved differently. That is the
   difference between a check that ran and a check that discriminated, and it is printed rather than asserted
   because a run whose two samples happen to agree in cost is not a defect in anything.

   WHAT ONE REPEAT CAN AND CANNOT DO, said plainly so `held` is never read as more than it is: a second sample
   can FALSIFY determinism and can never ESTABLISH it. What it buys is that the assumption stops being
   unmeasured and that a divergence which happens at all has somewhere to be seen. More samples are taken by
   running one document alone (`node engine/solvergate.mjs <doc>`); a repeat-count knob would be a bound wearing
   a flag, and this file has no use for one. */

/* THE ONE REPLY EVERY OWED REQUEST GETS, stated once. It is deterministic because the invariance being
   measured is over the SCHEDULE and nothing else: a reply whose body varied between runs would make every
   comparison below meaningless, and a per-document reply table would make this gate's input something someone
   maintains. §Learning-from-replies is what makes answering at all the right thing to do — a consumed reply is
   ALWAYS fetched, and its fields become concrete examples. */
const MOCK_BODY = JSON.stringify({
  ok: true, region: "us-east-1", limit: 7, features: { admin: false }, items: ["alpha", "beta"],
});

/* ─── the child: ONE document, ONE schedule, ONE process ────────────────────────────────────────────────────
   ONE PROCESS PER RUN, for engine/wpt.mjs's reason: a DCHECK is an abort, so a driver that ran the corpus in
   one process would report the first unbuilt capability and nothing after it. Per-run isolation makes an abort
   a RESULT for that run and leaves the rest of the picture intact. */

function gateFail(msg) {
  console.log("@GATEFAIL " + msg);
  process.exit(2);
}
/* AND THE SECOND FAILURE VOCABULARY, BECAUSE THE PARENT RENDERS EVERY `@GATEFAIL` AS `CORPUS:` AND TWO OF THE
   CHECKS BELOW ARE NOT ABOUT THE CORPUS AT ALL. The three refusals `gateFail` was written for — a host notice,
   an unanswerable host request, a frontier that cannot drain — each say "this document is the wrong document
   for this gate", and the reader's next act is to move or rewrite a fixture. A `qjs_emit_partial` that answers
   two different documents at one boundary is the opposite instruction: the fixture is fine and the ENGINE's
   render path has a side effect, so sending that reader to the corpus is a confident false attribution. This
   file already made exactly that argument about the streamed document and solved it by crossing `partial` to
   the parent rather than by borrowing the corpus sentence; a check that CANNOT cross its evidence needs the
   other half of it, which is a token of its own. `@ENGINEFAIL` cannot collide with `@GATEFAIL `: the parent
   matches that one on a trailing space and this one is a different word. */
function engineFail(msg) {
  console.log("@ENGINEFAIL " + msg);
  process.exit(2);
}

async function child(docPath, schedName) {
  const html = readFileSync(docPath, "utf8");
  const name = basename(docPath);
  const url = "https://gate.test/app/" + name;
  const factory = await import(WASM);
  const boot = factory.default ?? factory;

  /* The instance helper is engine/route.mjs's, spelled the same way, because it is the same ABI: allocate the
     C string, ccall, read the answer back as one. */
  async function instance() {
    /* THE PRINT SINK, TEED — the channel the extension reads findings off, given a reader here for the first
       time. `qjs_emit_partial` does not RETURN its document: it `printf`s one `@RESULT <json>` line, and
       extension/bridge.js scans the instance's captured lines for it, parses that one line and SPLICES IT OUT
       ("bound eng.lines growth"). This is that, with the same bound and for the same reason: the document
       GROWS with the finding set and main.c's own block records a leak of exactly the accumulated snapshots,
       so a driver that kept every line would re-create it on this side of the boundary. Everything is
       forwarded to console.log unchanged — the parent greps this child's stdout for `@RESUMED` and for
       `@GATEFAIL`, and a tee that swallowed a line would make the park schedule's resume check answer about a
       channel this function broke. Installed for EVERY schedule and not only the one that reads it: a second
       instance-construction path is precisely the silent divergence between two runs that this gate exists to
       catch, and it would sit inside the gate's own driver where nothing compares it. */
    const sink = [];
    const M = await boot({
      print: (s) => { console.log(s); if (String(s).startsWith("@RESULT ")) sink.push(String(s)); },
    });
    const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
    const str = (f, ...a) => String(M.ccall(f, "string", a.map(() => "number"), a.map(cs)) ?? "");
    /* §2.2.5's BODY, INTO THE INSTANCE'S LINEAR MEMORY — beside the record's JSON, never inside it. JSON
       cannot say a byte sequence, and each way of making it able to is an algorithm run by the zone that
       FETCHED: that is what Fetch §5.3 "Body mixin"'s `text()` was doing in safe-fetch.js (§5.2 stood here and
       is "BodyInit unions", which EXTRACTS a body rather than consuming one), and it is why HTML §8.1.4.2's
       classic-script decode had never once seen the bytes whose charset it exists to honour. The mock body is
       written as source text here, so this is an ENCODE. */
    /* THE REQUEST THIS ANSWERS IS THE PAIR the engine listed — `qjs_pending` answers
       `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>URL` and the reply is delivered against the (method, url) pair, so a GET and a POST to one address are two questions here. */
    const provide = (method, u, reply, body) => {
      const b = new TextEncoder().encode(body);
      const p = M._malloc(b.length + 1);
      M.HEAPU8.set(b, p);
      try { M.ccall("qjs_provide", "void", ["number", "number", "number", "number", "number"],
                    [cs(method), cs(u), cs(JSON.stringify(reply)), p, b.length]); }
      finally { M._free(p); }
    };
    /* ONE MID-RUN SNAPSHOT, TAKEN AND CONSUMED. `qjs_emit_partial` prints exactly one `@RESULT` line per call
       (main.c: unconditionally, and bridge.js DCHECKs its absence for that reason), and main.c is the ONLY
       printer of that token in the engine — so a drain that finds a number other than one is not a snapshot
       that is hard to locate, it is this channel carrying something nobody declared, and reading the LAST line
       the way a scanner would would make that invisible. Asked here rather than in the step loop so the take
       and the consume are one operation and no caller can leave a document behind for the next call to find. */
    const readOne = () => {
      sink.length = 0;
      M.ccall("qjs_emit_partial", "void", [], []);
      if (sink.length !== 1)
        gateFail(`qjs_emit_partial printed ${sink.length} \`@RESULT\` lines and it prints exactly one per ` +
                 "call — main.c is the engine's only writer of that token and extension/bridge.js reads its " +
                 "findings off this same channel, so a count other than one is the print sink between the " +
                 "engine and its host either dropping the document or carrying a second writer nobody declared");
      const line = sink[0];
      sink.length = 0;
      try { return { line, doc: JSON.parse(line.slice("@RESULT ".length)) }; }
      catch (err) {
        gateFail("the `@RESULT` line qjs_emit_partial printed is not JSON — the incremental merge does one " +
                 `JSON.parse of exactly this text, so this run would reach the trusted zone as a crashed ` +
                 `instance with every finding it had accumulated: ${String(err).slice(0, 120)}`);
      }
    };
    /* AND IT IS ASKED TWICE, WHICH IS THE ONLY DIRECT TEST OF THE ONE SENTENCE main.c WRITES OVER THIS ENTRY.
       That contract is "It READS: no flow is touched, nothing is drained, and the frontier the next step
       resumes is the one this was called on", and until this pair NOTHING anywhere asked it — the `stream`
       schedules test the contract's CONSEQUENCE (the finding set the run ends with) against the reference,
       which is a cross-schedule comparison and therefore cannot separate "asking changed something" from
       "these two schedules interleaved differently". Asked twice at ONE boundary with NO step between the two
       calls, it cannot: the frontier, the heap, the fork table and every counter are the same at both calls by
       construction, so the two documents are the same TEXT — not the same canonical SET, the same bytes,
       gauges included, with no DROP list and no canonicalization involved. A difference is a SIDE EFFECT in
       the compose, and result_json reaches into endpoint.c, solve.c, world.c, cold.c and concolic.c to build
       one, so the candidates are a latch consumed, a list drained or a counter reset while rendering.
       WHY IT IS WORTH THE SECOND CALL. extension/bridge.js's `streamPartial` calls this entry on a `PARTIAL_MS`
       cadence for the whole of any analysis that outlives it, so a side effect here is not a diagnostic
       curiosity: it is the product's own instrument changing the run it exists to observe, on every real page,
       which is §Testing's "an instrument that costs enough to shorten the run is measuring a different run"
       with the instrument shipped. This is the check for it and there is no other.
       IT IS ALSO A SELF-DISAGREEMENT CHECK AND NOT A STORED EXPECTATION — nothing is written down, the answer
       is carried by the run, and a solver that learns twice as much passes it unchanged: the only thing it can
       fail on is one call of one entry disagreeing with the call before it.
       AND THE COST IS STATED: it doubles the number of `qjs_emit_partial` calls on the two streaming schedules
       and on nothing else. That does not make `stream` differ from `direct` in more than the one call site the
       header holds fixed — the CALL COUNT was never the controlled variable there (the two streaming rows are
       3-8 against 24-26 asks of the same entry, which is the axis they are), and the whole claim under test is
       that the answer cannot depend on how many times the entry is asked. */
    const snapshot = () => {
      /* THE OWED LIST IS READ AROUND THE PAIR BECAUSE IT IS THE ONE WITNESS OF "NOTHING IS DRAINED" THAT THE
         DOCUMENT DOES NOT STATE. Everything else the contract promises is inside the two documents compared
         below; the host-owed list is not in `result_json` at all, so a drain there is invisible to every
         surface this gate has. What it would cost is not a missing field: an owed entry that disappears is a
         reply this host is never asked for and a flow parked on it for ever, and the frontier would then reach
         DONE around that flow — which this gate reports as `the frontier STALLED and this gate could fill
         nothing`, a sentence naming the CORPUS for a defect in the engine's render path. Reading it costs one
         string the step loop is about to compose anyway. */
      const owedBefore = str("qjs_pending");
      const first = readOne();
      const second = readOne();
      const owedAfter = str("qjs_pending");
      /* PER TOP-LEVEL FIELD, NOT AS ONE STRING. A whole-document text comparison is what this check was first
         written as, and it reported the byte offset of the first divergence — which named the ALLOCATOR
         FIGURE the compose itself had moved and said nothing about the fields the contract is actually over.
         Asked per field, the four self-measuring censuses leave by name (see INSTANT_CENSUSES) and every
         other field is held to exact equality, so what the check reports is the FIELD that moved. */
        {
        const keys = [...new Set([...Object.keys(first.doc), ...Object.keys(second.doc)])]
          .filter((k) => !INSTANT_CENSUSES.includes(k));
        const moved = keys.filter((k) => JSON.stringify(first.doc[k]) !== JSON.stringify(second.doc[k]));
        if (moved.length)
          engineFail("qjs_emit_partial was asked TWICE at one boundary, with no step between the two calls, " +
                     `and answered two DIFFERENT documents — ${moved.join(", ")} moved. main.c's contract ` +
                     "over that entry is that it READS: \"no flow is touched, nothing is drained, and the " +
                     "frontier the next step resumes is the one this was called on\". No flow ran between " +
                     "these calls, so every finding, every lifetime total, the frontier census and the " +
                     "quantum constants were the same at both and had to render identically; the four " +
                     "self-measuring censuses are already excluded by name, so this is not the compose " +
                     "measuring its own allocation. It is a SIDE EFFECT in the render path: result_json goes " +
                     "through endpoint.c, solve.c, world.c, cold.c and concolic.c, and one of them is " +
                     "consuming a latch, draining a list or resetting a counter as it renders. " +
                     "extension/bridge.js calls this on a `PARTIAL_MS` cadence for the whole of every long " +
                     "analysis, so whatever moved here moves on EVERY REAL PAGE and the run the product " +
                     "reports is not the run it would have made unobserved — §Testing's \"an instrument that " +
                     "costs enough to shorten the run is measuring a different run\", with the instrument " +
                     "shipped. This is NOT a corpus defect and NOT a schedule effect: one call of one entry " +
                     "disagreed with the call before it.\n" +
                     moved.map((k) => `           ${k}\n             call 1: ` +
                                      `${JSON.stringify(first.doc[k]).slice(0, 300)}\n             call 2: ` +
                                      `${JSON.stringify(second.doc[k]).slice(0, 300)}`).join("\n"));
      }
      if (owedBefore !== owedAfter)
        engineFail("the host-owed list changed across `qjs_emit_partial` — it was\n           " +
                   `${owedBefore.split("\n").filter(Boolean).join(" ; ") || "(nothing owed)"}\n         and ` +
                   `after the call it is\n           ${owedAfter.split("\n").filter(Boolean).join(" ; ") || "(nothing owed)"}` +
                   "\n         main.c says that entry READS and that \"nothing is drained\". An owed entry " +
                   "that disappears across it is a reply this host will never be asked for and a flow parked " +
                   "on it for ever; the frontier reaches DONE around that flow, and this gate then reports " +
                   "`the frontier STALLED and this gate could fill nothing` — naming the CORPUS for a defect " +
                   "in the engine's own render path. This is NOT a corpus defect.");
      return second.doc;
    };
    return { M, cs, str, provide, snapshot };
  }

  /* WHAT THE HOST OWES, and what it must REFUSE to owe. A corpus document that emits a host notice or a
     synchronous host request is not a harder document, it is the WRONG document for this gate: a notice means
     it created another instance's document (cold_park refuses to write a frontier holding a foreign world's
     segment, and would abort inside the `park` schedule), and an unanswered request parks a flow that the
     stall hook cannot see — the frontier would then reach DONE with that flow still blocked, which is a
     dropped flow this gate would report as a solver bug when it is a corpus one. Named as a corpus defect. */
  /* IT ANSWERS HOW MANY ENTRIES IT FILLED, which is the half the step loop below cannot get from the code
     alone: an engine that reports a STALL is owed something, and whether THIS round supplied it is what
     separates "the frontier has work again" from "nobody can supply this" (engine.h says so at the provider
     seam; engine_run's own driver makes the identical `filled == 0` test). */
  /* IT TAKES THE DECLARED POLICY AND THE ENGINE'S OWN CODE, never the schedule's NAME. The name is what the two
     reply decisions used to be read off, and one of them was not read off anything at all — it was inherited
     from how often the engine happened to return, which is the confound the header above measures. */
  function service(e, policy, stalled) {
    let paid = 0;
    for (const n of e.str("qjs_host_notices").split("\n").filter(Boolean))
      gateFail(`the document emitted the host notice \`${n.split("\t")[0]}\` — a corpus document for this gate ` +
               "is ONE origin-keyed instance with no peer, because cold_park refuses to write a frontier " +
               "holding a foreign world's segment. Move a cross-instance document to engine/route.mjs, which " +
               "is the driver that provisions a second instance");
    for (const l of e.str("qjs_host_requests").split("\n").filter(Boolean))
      gateFail(`the document asked the host to perform \`${l.slice(l.indexOf("\t") + 1).slice(0, 80)}\` — this ` +
               "gate answers no cross-agent operation, so the asking flow would stay parked while the frontier " +
               "reported DONE around it, and this gate would read that dropped flow as a solver regression");
    const pending = e.str("qjs_pending").split("\n").filter(Boolean);
    /* THE THREE REPLY POLICIES, ENUMERATED RATHER THAN DEFAULTED — a fourth spelling is a policy this loop
       cannot perform, and the gate would answer it by paying everything and calling that the schedule.
         "reported" — the whole list, in the turn it is reported. Zero latency.
         "stuck"    — the whole list, but only at the boundary where the engine reports it can make NO progress
                      at all. Holding it anywhere else is what keeps a flow's host-owed mark standing while the
                      pick is made, and that mark is the ONE input to flow_pick a host can move (see the header).
                      A STALL is where holding it buys nothing — no member can run, so there is no sibling to
                      hand the thread to — which is also why paying there can never manufacture the `paid === 0`
                      corpus failure below out of a policy choice.
         "last"     — one entry, tail first. The list is re-reported every turn until each entry is filled, so
                      answering the tail each time answers all of them in reverse order. */
    if (!["reported", "stuck", "last"].includes(policy.reply))
      gateFail(`the schedule declares the reply policy \`${policy.reply}\`, which this driver cannot perform — ` +
               "the three are enumerated at their consumer so a fourth is a schedule that does not run rather " +
               "than one that silently runs as the default");
    const answer = policy.reply === "last" ? pending.slice(-1)
                 : policy.reply === "stuck" && !stalled ? []
                 : pending;
    for (const line of answer) {
      const t = line.split("\t");
      if (t.length !== 5 || t.some((x, i) => i !== 1 && x === ""))
        gateFail("a pending line is not `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>URL` — " +
                 "qjs_pending joins the five and the reply is delivered against the (method, url) pair, so a " +
                 "short line makes a token the address. The empty DESTINATION is Fetch §2.2.5's own default " +
                 "and is the one field that may be empty");
      /* THE DESTINATION IS NOT NAMED, for route.mjs's reason: it is the class a reply may be INGESTED under
         and this gate ingests nothing — MOCK_BODY is minted here. Its vocabulary is asserted where it is
         written, at the join. */
      const [method, , initiator, provenance, u] = t;
      /* THIS GATE ANSWERS EVERY PARK WHATEVER IT SAYS ABOUT ITSELF, and that is what it MUST do: its subject
         is that one document's finding set is the same under several schedules, so a reply policy that varied
         with a request's provenance would be a fourth schedule the comparison cannot see. The fields are
         checked for their VOCABULARY only — a producer that drifts is a differential this gate would
         otherwise report as a solver disagreement. Firing policy is engine/trusted.mjs's.
         AND THE PROVENANCE IS PART OF WHAT THIS GATE MEASURES, WHICH IS WHY IT IS PARSED HERE RATHER THAN
         SKIPPED. It is composed at the park from the parking flow's own path (solver/flow.h's `path_forced`),
         so it is a per-flow fact reaching the wire — exactly the class of thing a schedule must not be able to
         change. Two schedules that disagree about it would disagree about which requests a zone may fire, and
         the differential's whole claim is that the schedule decides nothing. */
      if (initiator !== "parser" && initiator !== "script")
        gateFail(`a pending line states the initiator \`${initiator}\`, which is neither token ` +
                 "solver/engine.h declares — the vocabulary moved under every host that reads it");
      if (provenance !== "observed" && provenance !== "derived" && provenance !== "forced")
        gateFail(`a pending line states the provenance \`${provenance}\`, which is none of the three tokens ` +
                 "solver/engine.h declares — the vocabulary moved under every host that reads it");
      /* `computedType` IS THIS ZONE'S DECISION, AND WITHOUT IT THIS GATE COULD NOT MEASURE A REPLY AT ALL.
         The sniff belongs to whoever READ the bytes, so fetch_reply_computed_type asserts the field rather
         than defaulting it — and this record was written before that field existed. It is the same omission
         that stopped engine/route.mjs, left behind in the one driver whose whole subject is documents that
         consume replies: reply_values.html aborted on all four schedules and pin_and_shape.html on `park`,
         5 of 28 runs, every one inside the harness rather than in anything it was measuring. A `content-type`
         HEADER is not this field — the header is what the server SAID, this is what the zone DECIDED, which
         is the whole distinction the record keeps them apart for. The body below is MOCK_BODY, minted here. */
      const reply = { status: 200, statusText: "OK",
                      headers: [["content-type", "application/json"]],
                      urlList: [new URL(u, url).href], computedType: "application/json" };
      e.provide(method, u, reply, MOCK_BODY);
      paid++;
    }
    return paid;
  }

  /* ONE SESSION: seed (fresh, or from a residue), step to DONE, read the ONE result document, tear down. The
     teardown is not optional bookkeeping — main.c's teardown runs JS_RunGC and JS_FreeRuntime, whose
     gc_obj_list walk aborts on a leaked GC object, so every run of this gate is also a leak gate for the
     solver's own allocations. */
  async function session(recipes, sched, policy) {
    const e = await instance();
    /* THE DOCUMENT CROSSES AS A PAIR — a zero byte is legal in a document, and a `strlen` on this side would
       end the parse at the first one. The fixtures here are source text, so this is an ENCODE. */
    {
      const u8 = new TextEncoder().encode(html);
      const hp = e.M._malloc(u8.length + 1);
      e.M.HEAPU8.set(u8, hp);
      e.M.HEAPU8[hp + u8.length] = 0;
      /* EVERY FACT A DOCUMENT ARRIVAL CARRIES, STATED BY NAME AND NOT BY POSITION. This was two arrays kept
         in declaration order by hand — a fourteen-long type list and a fourteen-long value list — and every
         sentence about them was positional ("the second-last is", "the four after it are"), which is prose
         that goes wrong silently the moment the entry grows a parameter. It did: HTML §3.1.3's ancestor
         origins statement became the fifteenth, this gate stayed at fourteen, emscripten zero-filled the
         difference without a word, and every document under every schedule aborted. So the record is keyed by
         `content.mojom.Renderer.Init`'s own parameter NAMES now and engine/renderer_abi.mjs walks it: a
         parameter this record has no value for refuses BY NAME instead of shifting every later operand one
         slot, a key the interface declares no parameter of refuses too, and the operand count the walk
         produces is compared against what the BUILT GLUE declares the entry accepts.
         `inheritedCsp`/`inheritedCspSelfOrigin` ARE HTML §7.1.7's INHERITED POLICY CONTAINER, ABSENT: this
         gate roots each instance at a fixture document with no creator, so there is no container to clone and
         CSP §2.2.2's self-origin (this address's origin, which the entry derives) is the right one. The empty
         pair says that, rather than being an argument this driver forgot when the entry grew one.
         THE FOUR `inheritedCoep*` ARE §7.1.4's EMBEDDER POLICY of that same container, and they are NOT empty
         — they are the section's own "a new embedder policy", because §7.1.7 gives every container one and
         there is therefore no absence to spell. The two values are §7.1.4's token strings; main.c refuses one
         that names none of the three rather than reading it as the default.
         `parentNavigable` IS HTML §7.3.1.3's PARENT, `u` — this gate's fixture documents are rooted with no
         embedder, so their navigables are top-level traversables. It is the engine's own encoding for the
         absence rather than an empty string, because a navigable either has a parent or is a top-level
         traversable and both are facts a host states.
         `containerPolicy` IS Permissions Policy §9.5's CONTAINER, `null` — the same fact one algorithm over,
         and it is stated separately for the reason §7.3.1.3 defines the two links separately: a parent is a
         navigable, a container is the ELEMENT that presents it, and a document can be told about one without
         the other. §9.5 takes "null or an element (container)", and null is what this gate's fixtures are:
         it invents them, so nothing presents them, and §9.7 step 1 then returns "Enabled" for every feature.
         Stating it rather than letting the record default is the whole point — the engine refuses a record
         that states NOTHING for the container, because a silent absence and a stated null are different
         claims and only one of them is this gate's.
         `ancestorOrigins` IS HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST for the
         Document this instance builds — a THIRD statement about the same navigable and not a derivation of
         the two above it. §3.1.3's step 2 takes the Document's CONTAINER DOCUMENT and its step 3 returns the
         EMPTY output when there is none, which is what a top-level traversable's list is: `none` is that
         grammar's word for exactly that, a POSITIVE claim this gate is entitled to make about fixtures
         nothing embeds. An EMPTY FIELD IS NOT THE SAME CLAIM — it is a host that stopped writing the field,
         which the engine refuses, because reading silence as the empty list is what tells a cross-origin
         frame it is the top of its own tree, and no page can tell that from the truth.
         `creationSandboxFlags` IS HTML §7.1.5 "Sandboxing"'s CREATION SANDBOXING FLAG SET for that same
         navigable — a FOURTH statement about it, and the one this driver went short on next. §7.1.5's
         determine-the-creation-sandboxing-flags is "given null or an element embedder, return the union of the
         flags that are present in the following sandboxing flag sets: if embedder is null, then the flags set
         on browsing context's popup sandboxing flag set; if embedder is an element, then the flags set on
         embedder's iframe sandboxing flag set; [and] … on embedder's node document's active sandboxing flag
         set". This gate's fixtures have NO embedder — the same absence `parentNavigable` and `containerPolicy`
         each state one algorithm over — so only the first arm applies, and §7.1.5 says of that set: "Every
         top-level browsing context has a popup sandboxing flag set… When a browsing context is created, its
         popup sandboxing flag set must be empty. It is populated by the rules for choosing a navigable and the
         obtain a browsing context to use for a navigation response algorithm", and this driver runs neither.
         The union is EMPTY, and `none` is that grammar's word for it — a POSITIVE claim on `ancestorOrigins`'
         rule: core/frame/sandboxing.c's sandbox_flags_of_serialized refuses an empty field, because an empty
         one is a host that stopped writing it and reading that as the empty set is what would let a
         cross-origin `<iframe sandbox>` child run the scripts its embedder's markup forbids.
         IT IS STATED, NOT DERIVED FROM THE `u` PARENT BESIDE IT. main.c says outright that the two navigables
         its entry roots — a top-level traversable and one a PEER created — are told apart by nobody there, so
         a zero would be right for one of them and the absence of an answer for the other; each fact crosses on
         its own. */
      const operands = abiOperands("Init", "qjs_init", {
        document: [hp, u8.length], url, docId: name, headers: "", topLevelUrl: url,
        inheritedCsp: "", inheritedCspSelfOrigin: "",
        inheritedCoep: "unsafe-none", inheritedCoepEndpoint: "",
        inheritedCoepReportOnly: "unsafe-none", inheritedCoepReportOnlyEndpoint: "",
        parentNavigable: "u", containerPolicy: "null", ancestorOrigins: "none",
        creationSandboxFlags: "none",
      }, e.cs);

      /* THE TYPE LIST IS THE OPERAND LIST'S OWN LENGTH — a wasm operand is a number whatever the declared
         parameter was, so a literal count beside the call would be one more hand-kept copy of the fact that
         just went short. */
      e.M.ccall("qjs_init", "number", operands.map(() => "number"), operands);
      /* AND THE DOCUMENT'S BLOCK IS NOT FREED, WHICH IS A CHANGE AND NOT AN OVERSIGHT. This line was
         `e.M._free(hp)`, and `mojom.js` declares `document` as the ONE parameter of this interface the entry
         RETAINS: the bytes are not copied, the pointer is handed to the load that fills the parser's input
         byte stream (core/loader/html_document.c holds `html` and an offset and reads through it across the
         fills the load is cut into), so every fixture this gate ran was being tokenized out of freed memory
         from the second fill onward. It is emscripten's allocator that made that survivable and nothing else,
         which is exactly the shape a schedule-invariance oracle must not be built on: a reuse of that block
         between two fills is a difference between two runs of one document that no flow decided. The block is
         the instance's lifetime either way — the module is torn down whole. */
    }
    e.M.ccall("qjs_begin", "void", ["number"], [e.cs(recipes)]);
    /* THE HOST'S OWN LEVEL-1 YIELD, SET FROM THE DECLARED FLOOR AND NOT FROM A NAME. Nothing is dropped across
       it — engine.c returns YIELD with `g_sess_cur` held and the frontier untouched — which is exactly the
       claim under test: the next step must resume the SAME top flow on the byte-identical frontier. What it
       does NOT do is change which flow the next pick returns; that flow is `g_sess_cur`, carried across the
       return and handed to flow_pick as its seed, so the floor cannot move `_switches` (see the header).
       -Infinity IS SET RATHER THAN SKIPPED, because it is the engine's own default and passing it makes every
       schedule state its floor at the one line that sets one — a schedule that declares a floor this call does
       not make would be the same silent inheritance this file just removed. */
    e.M.ccall("qjs_set_yield_floor", "void", ["number"], [policy.floor]);
    if (sched === "park" && recipes === "")
      /* PARKED BEFORE THE FIRST PICK. engine_sched_slice takes the park at the top of the slice with no flow
         switched in, so this parks the frontier in its seeded state: one unstarted boot flow, whose recipe is
         a chain of no arms. That is the SHALLOWEST park there is, and it is deliberately the first one built,
         because it is the only one whose comparison needs no cross-session fold: session one emits NOTHING,
         so session two's findings must equal the reference EXACTLY.
         WHAT A DEEPER PARK NEEDS, named rather than left to be discovered: park after k steps and the findings
         SPLIT across two sessions, and their union is not a set union — endpoint.c merges by
         (method, path, param-name-set), unions each param's validValues, and REFINES a shape header to a
         concrete one. Two sessions each holding half of one merged record do not union back into it, and
         spelling that merge here would be a second copy of endpoint.c's rule, which is the defect CLAUDE.md
         names about every second speller. The fold belongs in ONE place and that place is not this file. */
      e.M.ccall("qjs_request_park", "void", [], []);
    /* THE THREE CODES, ENUMERATED. The ABI carries ENGINE_STEP_DONE (0), ENGINE_STEP_YIELD (2) and
       ENGINE_STEP_STALLED (3) — it used to fold the stall into the yield, and this loop inherited that: it
       read every non-zero code as "call me again" and so had no exit for a frontier that says it cannot
       progress. Against a document this gate cannot pay, that is not a slow run, it is a loop with no
       terminator at all. */
    let snapshots = 0;
    for (;;) {
      const r = e.M.ccall("qjs_step", "number", [], []);
      if (r === 0) break;   /* ENGINE_STEP_DONE — the frontier is empty (or was written out) */
      if (r !== 2 && r !== 3)
        gateFail(`qjs_step answered ${r}, which is none of DONE(0)/YIELD(2)/STALLED(3) — the ABI carries three ` +
                 "codes and this gate branches on all three, so a fourth is a contract that moved under it");
      /* THE HOST'S OWN BOUNDARY QUESTION, BEFORE THE REPLY AND NOT AFTER IT. extension/bridge.js's round asks
         for a snapshot of an engine that is HOT — one with owed replies outstanding, which is the state a
         fetching engine is in for most of a real analysis — so the snapshot is taken here, where this
         driver's engine is in that same state, rather than after `service` has emptied the owed list and left
         it in a state production rarely observes. The document is DISCARDED: what this schedule is testing is
         that ASKING changed nothing, and the answer to that is the finding set the run ends with, compared
         against the reference by the parent. What is not discarded is the check inside `snapshot` that one
         well-formed document arrived on the print sink, which is the half bridge.js asserts. */
      if (policy.partial) { e.snapshot(); snapshots++; }
      const paid = service(e, policy, r === 3);
      /* A STALL THIS ROUND DID NOT MOVE IS A DOCUMENT THAT CANNOT DRAIN, and draining is this gate's whole
         precondition: the differential compares the finding sets of a document whose frontier drains under
         several schedules, so a frontier parked on something no schedule will ever supply has no comparable
         answer to give. It is named as a CORPUS defect for the same reason the two above it are — this gate
         answers replies and nothing else, so the register it is stuck on says which capability the document
         wants and this gate does not have. */
      if (r === 3 && paid === 0)
        gateFail("the frontier STALLED and this gate could fill nothing — every member is parked on something " +
                 "only a host can supply and the only thing this one supplies is a reply, so the document " +
                 "cannot drain under any schedule and its finding set is not a function of the document " +
                 `alone. Owed: ${e.str("qjs_pending").split("\n").filter(Boolean).join(" ; ") || "(no fetch)"}`);
    }
    /* THE LAST SNAPSHOT IS TAKEN WITH THE FRONTIER ALREADY TERMINAL, WHICH IS WHAT MAKES IT COMPARABLE.
       Everywhere else in a run the two entries answer at different instants and a difference between them
       says nothing; here the loop has broken on DONE, no step runs between this call and the `qjs_result`
       below it, and both are `result_json(g_ctx)` — so on every surface this gate compares the two documents
       are the same document, and the parent holds them to that. What it can catch is not the C composer
       disagreeing with itself (one function, one caller each) but the two PATHS out of it: `qjs_result`
       returns a pointer this driver reads through the ccall binding, `qjs_emit_partial` prints a line this
       driver reads off the print sink, and the second is the channel extension/bridge.js's incremental merge
       is built on. A truncated line, a sink that drops under load, a
       `%s` that stops at an embedded byte: each of those makes the streamed half of the product report less
       than the engine found, and each is invisible to a driver that only ever asks for the pointer. */
    const partial = policy.partial ? e.snapshot() : null;
    const json = e.str("qjs_result");
    if (!json) gateFail("qjs_result answered nothing — the result document did not serialize");
    const out = JSON.parse(json);
    e.M.ccall("qjs_teardown", "void", [], []);
    return { result: out, partial, snapshots };
  }

  /* THE DECLARED TRIPLE, LOOKED UP ONCE AND ASSERTED — a child is re-entered by NAME (`--run <doc> <sched>`),
     so a name the parent's list carries and this map does not would run with no policy at all. Checked rather
     than defaulted, because the shape of that failure is the one this file is currently correcting: a run that
     produces a real-looking result under a schedule that never happened. The THIRD field is asked for by name
     rather than read as truthy, for the reason every row of POLICY states it: a schedule that omits it is a
     schedule whose boundary policy nobody stated, and `undefined` would run it as the one this file already
     had. */
  const policy = POLICY.get(schedName);
  if (!policy)
    gateFail(`the schedule \`${schedName}\` declares no (floor, reply, partial) triple — a schedule is that ` +
             "triple and nothing else, so a name without one is a run whose order nobody stated");
  if (typeof policy.partial !== "boolean")
    gateFail(`the schedule \`${schedName}\` states no boundary policy — every row of POLICY declares ` +
             "`partial` because this file has already been bitten once by a decision that was INHERITED " +
             "rather than stated, and an absent one would run as `false` while reading as a schedule that " +
             "had chosen it");

  let result, partial = null, snapshots = 0;
  if (schedName === "park") {
    const { result: first } = await session("", "park", policy);
    /* THE PRODUCER'S FIELD IS CHECKED, NEVER DEFAULTED — extension/bridge.js keeps the same DCHECK, and
       CLAUDE.md's rule is that a name read somewhere and written nowhere is a broken contract a `||` turns
       into a plausible datum. */
    if (!Array.isArray(first._park))
      gateFail("the result document of a PARKED session carries no `_park` array — the residue is what the " +
               "host writes to IndexedDB and hands back to qjs_begin, so an absent one is a frontier that was " +
               "dropped rather than paged out");
    if (first._park.length === 0)
      gateFail("the park wrote an EMPTY residue over a frontier that had a seeded flow in it — an empty park " +
               "document tells the host this engine was fully explored, so the whole frontier would be deleted");
    /* ASKED OF THE ONE SURFACE LIST, never of a second copy of it. This loop named its surfaces literally
       while SURFACES named one more, so a surface emitted before the first pick would have satisfied a
       precondition that had never looked at it — the same field, unclassified in one place and invisible in
       the other. `surfaceSet` answers for either shape, so the emptiness question is asked once.
       AND IT IS THE ACCUMULATING SURFACES IT IS ASKED OF, which is the declaration's second fact and not a
       filter this loop invented. The question here is whether the parking session EMITTED something the
       resumed session's comparison would have to FOLD, and a reading of the terminal instant is not emitted
       output: `_wfq` is the census of the residue whose NON-emptiness the check three lines above REQUIRES,
       so asking one session for a live frontier and for no members standing is asking it for two
       contradictory things. */
    for (const [surface, d] of SURFACES)
      if (d.accumulates && surfaceSet(first, surface).length !== 0)
        gateFail(`the session parked before its first pick still emitted ${surfaceSet(first, surface).length} ` +
                 `${surface} — this gate compares the RESUMED session against the reference on its own, which ` +
                 "is sound only while the parking session emits nothing. It emitted something, so the two " +
                 "sessions' findings now have to be FOLDED, and that fold is endpoint.c's merge rule (see the " +
                 "park comment above): build it in one place before widening this schedule");
    ({ result, snapshots } = await session(first._park.join(";"), "park", policy));
  } else {
    ({ result, partial, snapshots } = await session("", schedName, policy));
  }
  /* THE TWO DOCUMENTS CROSS TOGETHER AND ARE COMPARED IN THE PARENT, where the report vocabulary is. A child
     says @GATEFAIL and the parent renders every one of those as `CORPUS:` — which is right for the three
     refusals above it and would be a confident false attribution here, because a streamed document
     disagreeing with the returned one is a fact about the ENGINE's two output paths and about this driver's
     capture of the second, and naming the corpus for it would send the reader to rewrite a fixture. `partial`
     is `null` for every schedule whose boundary policy is false, which is a POSITIVE statement that the
     question was not asked — never an empty document, which would compare as a run that streamed nothing. */
  console.log("@EMIT " + JSON.stringify({ document: name, schedule: schedName, result, partial, snapshots }));
}

/* ─── canonical form ────────────────────────────────────────────────────────────────────────────────────────
   THE SET, NOT THE SEQUENCE. Emission ORDER is the schedule's to choose — which flow emitted first is exactly
   what a different schedule changes — so every array is sorted by its own canonical text and every object's
   keys are sorted. What survives is the SET of findings, which is what the invariant is about.
   A DROPPED FIELD IS DROPPED BY PATH, so the exclusion is stated about one place in one document rather than
   about a name that might mean something else somewhere else. */
/* THE FOUR READINGS OF AN INSTANT, NAMED ONCE BECAUSE THEY NOW HAVE TWO READERS. The census row inside DROP[""]
   below carries the whole argument for why they are dropped from the cross-schedule comparison and it is
   unchanged; what is new is that `snapshot` needs the SAME four and needs them for a DIFFERENT reason, so the
   list is declared here and spread into that row rather than typed a second time. This is `SURFACES_ACCUMULATING`'s
   pattern — one declaration, two readers — and it is used for its reason: a hand-kept second copy agrees with
   the first until the day it does not, and the disagreement is silent in the direction that reads as a pass.
   THE SECOND READER'S REASON, WHICH IS NOT THE FIRST'S. The cross-schedule row drops them because their value
   is a reading of whatever instant the document was composed at and two schedules reach that instant having
   allocated and forked differently. `snapshot` excludes them because THE COMPOSE ITSELF MOVES THEM: asked
   twice at one boundary with no step in between, `result_json` allocates to build the first document and the
   second document's `_heap`/`_swap` then report the larger arena. That is MEASURED and not predicted —
   flag_fork.html under `stream` answered `"arenaKiB":1754` then `"arenaKiB":1758`, and under `eagerstream`
   1746 then 1750, on the build stamped f6cbdd9b — and it is not a violation of anything: main.c's contract
   over that entry promises that no flow is touched, that nothing is drained and that the frontier resumes as
   it was, and a self-measuring allocator figure is none of the three. So these four leave the two-call
   comparison and EVERYTHING ELSE stays in it, `_wfq` and `_quantum` included: the compose creates no flows and
   `_quantum` is three compile-time constants, so neither has anything for an observer to perturb. */
const INSTANT_CENSUSES = ["_cold", "_heap", "_swap", "_forkAt"];
const DROP = new Map([
  ["", new Set(["_switches", "_flows", "_candidates", "_jobsQueued", "_jobsRun", "_unitsDone",
                "_worldSegmentsHeld", "_worldSegmentsMade", "_worldSegmentsForked", "_park",
                "_sourceReads", "_sinkReached", "_sinkTainted", "_sinkSuppressed",
                /* Counted once per (routed record, timeline offered it), so their sum is the number of flows
                   standing when the host routed — see the header. Zero in this gate by construction: nothing
                   here calls qjs_route. */
                "_routedDelivered", "_routedRefused",
                /* §9.3.3 step 8's four task ends, each counted per queued task PER ARM — engine.h's
                   conservation law is an inequality for exactly that reason. */
                "_routedTasksFired", "_routedTasksTargetOrigin", "_routedTasksTargetGone",
                "_routedTasksThrew",
                /* THE ORPHAN CENSUS, AND `_orphansDriven` IS ARGUED IN RATHER THAN WAIVED. `_orphansAsked`
                   counts the times a flow reached the end of its OWN work, which is the schedule's to choose
                   and never converges — the ground `_switches` is dropped on. `_orphansDriven` looks like it
                   converges on a draining frontier (every uncalled function is eventually taken, and the bit
                   is consumed once per session), and it still does not: a take can ROUTE to a flow already
                   waiting for that body instead of driving a fresh one, and which of those a schedule reaches
                   first decides whether the count moves. It is a count of EVENTS either way, which is the
                   arrival census's ground. WHAT IS HELD INVARIANT IS THE FINDING THE DRIVE PRODUCES — the
                   endpoint lands in `fetchCallSites`, which is compared — so a schedule that drove the same
                   uncalled code and learned the same endpoints passes whatever these two say, and one that
                   learned less fails on the surface rather than on the counter. */
                "_orphansDriven", "_orphansAsked",
                /* THE FOUR CENSUSES, AND THEY ARE DROPPED ON A GROUND NOTHING ELSE IN THIS LIST IS. Every
                   name above is a COST — a total over the run whose magnitude the schedule chooses. These are
                   not totals at all: each is a READING OF AN INSTANT, taken at whatever moment the document
                   was composed at, of the frontier's live size, the runtime's live heap, the C allocator's
                   live arena and the fork table. Two schedules that learn the identical findings reach that
                   instant having allocated differently, switched differently and forked in a different order,
                   so holding any row of them invariant would fail every schedule against the reference ON
                   HEALTHY CODE — the false red §Testing names, manufactured by the gate itself.
                   `_wfq` IS NOT IN THIS LIST AND THAT IS NOT AN OVERSIGHT: it is the one census with an EMPTY
                   shape, and the document this gate compares is the terminal one, which a session reaches by
                   draining or parking — both of which leave no members standing. So its value is `{members:0}`
                   under every schedule and comparing it is free. The other four have no empty shape (result.h
                   says why), so there is no instant at which they agree by construction.
                   WHAT IS HELD INVARIANT INSTEAD IS WHAT THE WORK PRODUCED — `fetchCallSites` and
                   `securitySinks` — so a schedule that paged more, forked elsewhere or held a bigger heap and
                   still learned the same surface passes, and one that learned less fails on the surface rather
                   than on a byte count. WHAT IT COSTS IS STATED PLAINLY: this gate cannot catch a census
                   itself regressing, and nothing else can either — the DCHECKs in solver/result.c and the
                   shape asserts in extension/bridge.js are what stand there. */
                /* Declared above INSTANT_CENSUSES and spread here, because `snapshot` reads the same four for
                   a different reason — see that declaration. */
                ...INSTANT_CENSUSES,
                /* THE ONE COST IN THIS SET THAT IS NOT A COUNT, AND IT IS DROPPED ON THE SPEC'S OWN GATING
                   RATHER THAN ON A MAGNITUDE. Every other name above is a total or a reading of an instant
                   whose SIZE the schedule chooses, and that reason does not reach a LIST OF MESSAGES — a
                   reader who assumes it does will misclassify the next field of this kind, which is why the
                   argument is written out rather than filed under the census paragraph.
                   WHAT DECIDES IT IS WHICH OF TWO QUEUED TASKS RAN FIRST. HTML §8.1.4.7 "Unhandled promise
                   rejections" step 4 is "Queue a global task on the DOM manipulation task source given global
                   to run the following step", and the report lives INSIDE that task: step 4.1.1 is "If
                   p.[[PromiseIsHandled]] is true, then continue." So a `.catch` attached before that task runs
                   means the rejection is never reported and there is nothing to take back, while the identical
                   `.catch` attached after it means step 4.1.3 has already reported and HTML §8.1.6.4
                   "HostPromiseRejectionTracker(promise, operation)" step 7.4 queues the retraction that empties
                   the row. Which of the two happens is which of two queued tasks the scheduler picked first,
                   and CLAUDE.md §scheduler makes every enqueued job a first-class flow in the one WFQ — so it
                   is precisely what these schedules vary. `var p = f(); p.catch(h)` is the ORDINARY shape, not
                   a corner: it is why the standard keeps two lists at all, and browser/core/html/
                   unhandled_rejection.c widens the gap further by queueing one task per promise.
                   THE VARIANCE IS CONFINED TO THIS ARRAY BY CONSTRUCTION AND NOT BY LUCK, which is the whole
                   reason `pageErrors` can stay in SURFACES while this leaves it. The two are DISJOINT —
                   solver/result.c's errs_json_array_where emits a message here only when NO occurrence of it
                   still stands — so a report that is made and then withdrawn contributes NOTHING to
                   `pageErrors`, and moving the handler earlier or later moves a message between ABSENT and
                   THIS ARRAY without ever touching that one. See `pageErrors` in SURFACES for the premise that
                   confinement rests on and the one shape that breaks it.
                   WHAT IT COSTS, STATED BECAUSE IT IS REAL AND NOT A ROUNDING: solver/result.c says in as many
                   words that a retracted row is still a capability the page reached for, so this gate stops
                   asking whether such a gap is seen the same way under every schedule. It cannot ask — holding
                   this invariant would fail healthy documents whose only sin is attaching a handler in a later
                   task, which is the false red §Testing names, manufactured by the gate itself.
                   BY CONSTRUCTION AND NOT FROM A MEASUREMENT, stated for the reason the rows below state it:
                   no document in engine/tests/solver registers an `unhandledrejection` listener, and every
                   `.then` in the corpus is attached in the expression that produced the promise, so this array
                   is `[]` on all of them today. Nobody may read this row as evidence that a mismatch in it was
                   ever observed here. */
                "pageErrorsRetracted",
                /* AND THE THIRD ARRAY, WHICH LEAVES ON THE ONE ABOVE'S GROUND AND NOT ON ITS OWN — it INHERITS
                   the variance rather than having any. `pageErrorsExplored` names the messages the engine
                   MINTED, and whether a given (message, throw site) pair was minted by this engine is decided
                   by the PRODUCER at the raise, which no schedule touches; so on any message that is reported
                   under every schedule, this array's answer is invariant.
                   WHAT IT IS NOT INVARIANT OVER IS WHETHER THE MESSAGE IS REPORTED AT ALL. It is ORTHOGONAL to
                   the standing/retracted pair — solver/result.c composes it from every reported row regardless
                   of which of the two that row is in — so a message whose only occurrence is RETRACTED enters
                   and leaves this array exactly as it enters and leaves `pageErrorsRetracted`, for exactly the
                   reason written above: HTML §8.1.4.7 step 4.1.1 suppresses the report for a promise handled
                   before its queued task runs and §8.1.6.4 step 7.4 withdraws it for one handled after, and
                   which of those happens is which of two queued tasks the scheduler picked first.
                   SO THE STANDING HALF OF IT WOULD BE COMPARABLE AND THE ARRAY AS A WHOLE IS NOT, and that is
                   stated rather than acted on: splitting the surface to recover the comparison would put the
                   document's shape back into a gate's convenience, and this gate's job is to compare what the
                   engine emits. The FINDING that half would protect is already held — an exploration throw
                   changes no `fetchCallSites` row, since the sibling arms of the same fork carry on past it
                   and emit exactly those endpoints, which is the whole argument result.h makes for the
                   array existing.
                   BY CONSTRUCTION AND NOT FROM A MEASUREMENT, on the same ground as its neighbour, and stated
                   as what was READ rather than as an absolute: `result_explored_throw` has one caller, the
                   `options.signal` arm of browser/core/events/event_target.c, and no document in
                   engine/tests/solver passes an options OBJECT to `addEventListener` at all — so this array is
                   `[]` on the whole corpus today and nobody may read this row as evidence that a mismatch in
                   it was ever observed here. */
                "pageErrorsExplored"])],
  /* `turns` IS A SWITCH-IN COUNT, WHICH IS `_switches` ONE LEVEL DOWN. solve.c counts it in solve_flow_begin —
     the scheduler's every switch-in of a candidate flow — and says so where it counts it: "IT IS SWITCH-INS AND
     NOT DISTINCT FLOWS, which is what makes it a scheduling fact rather than a second copy of `tried`: a
     candidate preempted and resumed twenty times has been given twenty turns". The schedules above choose it:
     a reply held while a sibling runs (`preempt`) and a reply answered in the turn it was reported (`eager`)
     give the same document a different number of switch-ins, and holding it invariant would fail one of them
     against the reference on healthy code — which is the one ground `_switches` is dropped on and the same
     ground here. THE SENTENCE THIS REPLACES SAID `preempt` "is DEFINED as outranking the running flow after
     every flow step, so it maximises exactly this number by construction", AND THAT WAS FALSE ABOUT THE
     MECHANISM: the yield floor is a HOST yield that ends the slice and hands the same flow straight back
     (header), so it outranks no flow and maximises no switch count. The row is right; its old reason was not,
     and a right row resting on a wrong reason is the shape that survives review. Its NEIGHBOURS stay compared and that is the
     line: `reached`, `survived`/`survivedOf`, `escaped` and `fires` are observations of what a re-execution got
     through the page's own transforms, and on a frontier that DRAINS every seeded candidate runs, so each of
     them converges to a fact about the document. A count of how many times work was switched in never
     converges, because the schedule chooses it. THE ARGUMENT IS BY CONSTRUCTION AND NOT FROM A MEASUREMENT,
     which is stated rather than implied: this is a classification made before the false red rather than after
     one, so nobody should read the row as evidence that a `turns` mismatch was ever observed here. */
  /* `substituted` AND `sinkStrings` ARE THE SAME ROW AS `turns`, AND FOR THE SAME REASON READ ONE LEVEL IN.
     Both are OBSERVATION COUNTS rather than observations: solve.c raises `substituted` at every substitution
     concolic_deliver performs and `sinkStrings` at every string a code-execution sink is handed while one is
     live. What makes them schedule-chosen is not preemption — a preempted flow RESUMES and delivers nothing
     twice — it is the RESTART: concolic_clear_pins clears the component's delivered flag when a flow is
     entered fresh, and cold.c drops the candidate's ladder on a park deliberately so a resumed candidate
     re-proves itself (solver/flow.h says so at `cand_surv`/`cand_rung`). The `park` schedule therefore replays
     a candidate that has already delivered, and it delivers again — so the same document reports a different
     count under a schedule that parks than under one that does not, on healthy code.
     THEIR NEIGHBOURS STAY COMPARED AND THE LINE IS THE ONE THE ROW ABOVE DRAWS: `reached`, `survived`/
     `survivedOf`, `escaped` and `fires` are what a re-execution GOT THROUGH the page, and on a frontier that
     drains each converges to a fact about the document; a count of how many times the work was performed
     never converges, because the schedule chooses how many times it is performed.
     BY CONSTRUCTION AND NOT FROM A MEASUREMENT, stated for the reason the row above states it: this
     classification was made when the two fields were added, before any run, so nobody may read it as evidence
     that a mismatch in either was ever observed here. */
  /* `resumed` IS THE SAME ROW AGAIN AND IT IS THE PUREST CASE OF IT — a count of how many of this search's runs
     were rebuilt out of the cold tier. The `park` schedule is DEFINED as the one that parks a frontier and
     rebuilds it in a second session (this file requires `@RESUMED > 0` from it, above), and no other schedule
     resumes anything at all, so this number is not merely schedule-INFLUENCED like its neighbours: the
     schedule is its entire content. Holding it invariant would fail `park` against the reference on healthy
     code, every time, by construction.
     ITS NEIGHBOURS STAY COMPARED FOR THE REASON THE TWO ROWS ABOVE GIVE, and `payloads`/`probes` are the ones
     to watch here rather than to drop: a resumed candidate registers its search before any exploration flow of
     the new session detects one, and until add_pending's `opened` latch that ORDER decided whether the search
     was ever opened — so the resuming session built no probe, derived nothing, and reported `payloads:[]`
     where the reference reported a probe and its escapes. That is a divergence this gate is exactly right to
     see, and the fix belongs in solve.c rather than in this set. BY CONSTRUCTION AND NOT FROM A MEASUREMENT,
     stated for the reason the rows above state it. */
  [".securitySinks[]", new Set(["tried", "resumed", "turns", "substituted", "sinkStrings"])],
]);
function canonStr(v, path) {
  if (Array.isArray(v)) {
    const parts = v.map((e) => canonStr(e, path + "[]"));
    parts.sort();
    return "[" + parts.join(",") + "]";
  }
  if (v && typeof v === "object") {
    const drop = DROP.get(path);
    const keys = Object.keys(v).filter((k) => !(drop && drop.has(k))).sort();
    return "{" + keys.map((k) => JSON.stringify(k) + ":" + canonStr(v[k], path + "." + k)).join(",") + "}";
  }
  return JSON.stringify(v);
}
/* Per SURFACE, so a failure names WHICH one disagreed and by which elements. One canonical string for the
   whole document would say only that something differs, which is the report nobody can act on.
   EACH SURFACE DECLARES ITS SHAPE, because assuming one is how this list went wrong. The previous declarer of
   `map` was `probeResults`, error-derived request schemas keyed by the `<METHOD> <host><path>` identity the
   Send panel resolves a request body with, and the first run of this gate reported it as an unclassified field
   on all five corpus documents because the coverage check asked `Array.isArray` of a surface that is not one.
   That surface is gone from result.c: an API's rejection is the answer to a DELIBERATELY MALFORMED request,
   which this engine cannot issue, so it is read in the trusted zone (extension/lib/req2proto.js) and never
   crosses the seam this gate compares. Its row is deleted with it rather than left asserting a field no writer
   produces.
   AND `_wfq` IS THE DECLARER NOW, WHICH THIS FILE ALREADY CLAIMED AND DID NOT HAVE. The census row above says
   in as many words that `_wfq` "IS NOT IN THIS LIST AND THAT IS NOT AN OVERSIGHT … comparing it is free", and
   that was a claim about a mechanism with no row anywhere: `checkCoverage` requires every key of the result
   document to be a SURFACE or a named cost, so an unclassified `_wfq` FAILED every document under every
   schedule with the message telling the reader to classify it. Two prose paragraphs asserting the
   classification and no line performing it is the read-with-no-writer defect inverted — a rule stated twice
   and enforced nowhere — and it sat behind an earlier abort where nobody could see it. The argument is the
   census row's, unchanged: the document this gate compares is the TERMINAL one, which a session reaches by
   draining or parking, and both leave no members standing, so `{members:0}` is the value under every schedule
   and a schedule that left work on the frontier says so HERE instead of in a cost nobody reads.
   AND A SURFACE DECLARES A SECOND FACT, WHICH `_wfq` IS THE FIRST TO ANSWER DIFFERENTLY. `accumulates` is
   whether a surface's value is EMITTED OUTPUT the run adds to — a finding, which a session boundary can split
   in half and which two sessions would then have to FOLD — or a READING OF THE TERMINAL INSTANT, which the
   session that reaches that instant answers alone and which nothing folds. The `park` schedule is where the
   difference is load-bearing and it is not a distinction invented here: three lines above its emptiness
   pre-check, the same schedule REQUIRES the parking session's residue to be NON-empty ("the park wrote an
   EMPTY residue over a frontier that had a seeded flow in it"), and `_wfq` is the census of exactly that
   residue. Asking one session to have a live frontier and no members standing is asking it for two
   contradictory things, so the pre-check asks about the surfaces that ACCUMULATE and the comparison below
   asks about all of them — one list, two questions, neither of them a second literal copy. */
const SURFACES = new Map([
  ["fetchCallSites", { shape: "array", accumulates: true }],
  ["securitySinks",  { shape: "array", accumulates: true }],
  /* `pageErrors` SURVIVES THE ARGUMENT THAT DROPPED `pageErrorsRetracted`, AND THE PREMISE IT SURVIVES ON IS
     WRITTEN DOWN HERE BECAUSE IT IS NOT UNCONDITIONAL. The tempting inference is that a schedule-dependent
     RETRACTION set makes the ERROR set it corrects schedule-dependent too, and that inference is wrong: the two
     arrays are disjoint terminal readings, not a set and its correction. On a run that DRAINS, a message stands
     here iff the page reached for something and NOTHING ever handled it. HTML §8.1.4.7 "Unhandled promise
     rejections" step 4.1.1 — "If p.[[PromiseIsHandled]] is true, then continue." — suppresses the report for a
     promise handled before its queued task runs, and HTML §8.1.6.4 "HostPromiseRejectionTracker(promise,
     operation)" step 7.4 withdraws it for one handled after; both of those end at standing zero, and
     solver/result.c emits a message here only while an occurrence still stands. So WHEN the handler attached
     moves a message between ABSENT and `pageErrorsRetracted` and never into or out of this array. That is what
     makes exactly one of the pair invariant, and it is this one.
     THE ONE SHAPE THAT BREAKS IT IS THE PAGE'S OWN LISTENER, NAMED SO A FUTURE MISMATCH HAS A FIRST HYPOTHESIS
     INSTEAD OF A CAP HUNT. Step 4.1.3 — "If notCanceled is true, then the user agent may report
     p.[[PromiseResult]] to a developer console." — is gated on the CANCEL rather than on handled-ness, so a
     page that registers a `preventDefault`-ing `unhandledrejection` listener IN A TASK THAT RACES THE NOTIFY
     TASK is reported under one task order and not under another, and this array would then legitimately differ.
     It is not dropped for that: a listener is normally registered before anything rejects, so the race is
     exotic, and a mismatch reached that way is worth SEEING — dropping a surface to pre-empt a red nobody has
     seen is the reflex CLAUDE.md bans. DERIVED FROM THE SPEC'S GATING AND NOT MEASURED: no corpus document has
     such a listener, so this shape has never fired here and this paragraph is a hypothesis to test first, never
     a report that it did. */
  ["pageErrors",     { shape: "array", accumulates: true }],
  ["_wfq",           { shape: "map",   accumulates: false }],
  /* `_quantum` IS COMPARED, AND IT IS THE ONE FIELD ON THIS DOCUMENT FOR WHICH THAT IS FREE BY CONSTRUCTION
     RATHER THAN BY ARGUMENT. The four censuses are dropped because each is a READING OF AN INSTANT whose value
     the schedule chooses — hold any row of them invariant and every schedule fails against the reference on
     healthy code. `_wfq` escapes that by an argument about WHICH instant (the terminal one, reached by
     draining or parking, so `{members:0}` under every schedule). This one needs neither argument: it is not a
     reading at all. solver/quantum.c's `quantum_json` renders `ENGINE_QUANTUM_MS` and two string literals of
     its own branch, all three compile-time constants of the artifact — so no instant, no allocation, no fork
     order and no reply policy can move it, and there is no schedule under which the reference and the subject
     could legitimately differ.
     AND A MISMATCH WOULD BE A REAL FINDING, WHICH IS WHY DROPPING IT WOULD COST SOMETHING. This gate's whole
     claim is that the SCHEDULE decides nothing; every schedule here is the same wasm driven through the same
     ABI in the same process, so two of them disagreeing about the denomination is not a solver difference at
     all — it is two different programs or two different hosts being compared while the report says one
     revision was measured. That is precisely the confident false comparison the field was added to prevent,
     arriving in the instrument built to prevent it. engine/build.mjs already throws when two `@QUANTUM` lines
     in ONE run disagree, for the identical reason; this is that check on the surface the shipped path writes.
     `accumulates: false` for `_wfq`'s reason and one better: it is not emitted output that a session boundary
     could split, and the `park` schedule's two sessions are one binary on one host, so both answer the same
     three constants and there is nothing for the fold to reconcile. */
  ["_quantum",       { shape: "map",   accumulates: false }],
]);
/* DERIVED FROM THE DECLARATION, never restated beside it. `accumulates` is already the field that separates
   emitted output from a reading of the terminal instant, and it has two readers now — the park schedule's
   fold pre-check and the no-subject check below — so the list they share is taken from the one declaration
   rather than typed a second time. A surface added to SURFACES joins both readers with no edit, which is the
   property the classification has to have: the alternative is a hand-kept list that agrees with the map until
   the day it does not, and the disagreement would be silent in exactly the direction that reads as a pass. */
const SURFACES_ACCUMULATING = [...SURFACES].filter(([, d]) => d.accumulates).map(([k]) => k);
const shapeOk = (v, d) => d.shape === "array" ? Array.isArray(v)
                                              : (!!v && typeof v === "object" && !Array.isArray(v));
/* AND THE TWO LISTS ARE CHECKED AGAINST THE DOCUMENT, which is what makes "compared by default" a mechanism
   rather than a claim in a comment. A field result.c adds that is in NEITHER list is a finding surface nobody
   compares or a cost nobody named, and both are silent: the gate would keep passing while measuring less than
   it says it does. It is the same check engine/build.mjs makes between QJS_ABI and main.c's QJS_EXPORT
   entries, for the same reason — a list and the thing it describes are one fact and must be asked, not
   restated. */
/* BOTH DIRECTIONS, because either one alone is silent. A key in NEITHER list is a surface nobody compares; a
   SURFACE that is not in the document is a comparison over nothing, which reads as agreement. Asked once per
   run, so surfaceSet below can assume what it is handed — the impossible state is made impossible here rather
   than defended against there. */
function checkCoverage(doc, sched, result) {
  const cost = DROP.get("");
  const unknown = Object.keys(result).filter((k) => !SURFACES.has(k) && !cost.has(k));
  const absent = [...SURFACES].filter(([k, d]) => !shapeOk(result[k], d))
                              .map(([k, d]) => `${k} (${d.shape})`);
  if (!unknown.length && !absent.length) return true;
  if (unknown.length)
    console.log(`  FAILED ${doc} [${sched}]\n         the result document carries ${unknown.join(", ")}, which ` +
                "this gate neither compares as a finding surface nor names as a schedule-dependent cost. " +
                "Classify it in engine/solvergate.mjs: a FINDING goes in SURFACES and is held invariant across " +
                "schedules; a COST goes in DROP[\"\"] with the reason it legitimately differs. Until then the " +
                "gate is measuring less than it claims to.");
  if (absent.length)
    console.log(`  FAILED ${doc} [${sched}]\n         the result document has no ${absent.join(", ")} — ` +
                "result.c composes every one of them in a single snprintf, so an absent one is a contract this " +
                "gate and the engine no longer share, and comparing it would compare nothing and call that " +
                "agreement. The shape is named beside the field because a surface of the wrong shape is the " +
                "same silence as an absent one.");
  return false;
}
/* THE SET, WHATEVER THE CONTAINER. An array surface's element is its entry; a MAP surface's element is its
   (key, value) PAIR, because the key is half the finding — a schema filed under the wrong endpoint identity is
   a different finding, not the same one moved. Both come back as a sorted list of canonical strings, so the
   comparison below is one operation over both and never asks which it is holding. */
function surfaceSet(result, surface) {
  const v = result[surface];
  if (SURFACES.get(surface).shape === "array") return v.map((e) => canonStr(e, "." + surface + "[]"));
  return Object.keys(v).sort().map((k) => JSON.stringify(k) + ":" + canonStr(v[k], "." + surface + "{}"));
}

/* ─── the driver ────────────────────────────────────────────────────────────────────────────────────────────*/

if (process.argv[2] === "--run") {
  await child(process.argv[3], process.argv[4]);
  process.exit(0);
}

if (!existsSync(WASM)) {
  console.error("[solvergate] the shipped engine is not built — this gate drives the production ABI:\n" +
                "  node engine/build.mjs");
  process.exit(1);
}
if (!existsSync(CORPUS)) { console.error("[solvergate] no corpus at " + CORPUS); process.exit(1); }

/* WHICH TREE — AND, HERE, WHICH BUILD. This gate is the one that does not compile: it imports the shipped
   wasm, so the revision above it in the log is a fact about the SOURCES and not about the program that
   answered. Both are printed, and an artifact older than its own cone says so by name (engine/gate_revision.mjs)
   — a clean tree in front of a stale .wasm is the pair that reads most convincingly and is most wrong.
   AFTER the child re-entry above, so a spawned schedule does not print the block its parent already did. */
const REV_AT_START = gateRevision(["engine/host", "engine/qjs", "engine/solvergate.mjs", "engine/gate_revision.mjs"], WASM);
for (const l of revisionLines(REV_AT_START)) console.log(l);

/* IS THE PROGRAM THAT ANSWERED A BUILD OF THE TREE THAT CLASSIFIED IT — asked as a BOOLEAN because exactly one
   check below is undecidable without it. engine/gate_revision.mjs's own comment states the three states this
   collapses ("stamped and EQUAL to this tree -> the artifact is a build of the revision above"), and this reads
   the same fields it stamps rather than re-deriving them: an artifact with no stamp, one stamped at another
   revision, or one built from a tree that was dirty is not a build of THIS revision, and the difference
   between the classification list in this file and the document that program emits is then a fact about
   neither of them. It is not used to soften any other verdict — the run itself is measured either way. */
const ARTIFACT_IS_THIS_REVISION =
  !!REV_AT_START.stamp && REV_AT_START.stamp.head === REV_AT_START.head &&
  REV_AT_START.stamp.qjsHead === REV_AT_START.qjsHead &&
  !REV_AT_START.dirty.length && !REV_AT_START.unasked.length &&
  !(REV_AT_START.stamp.dirty || []).length && !(REV_AT_START.stamp.unasked || []).length;

/* AND THE COST LIST IN THE OTHER DIRECTION, WHICH IS `checkCoverage`'S OWN RULE APPLIED TO THE HALF IT SKIPPED.
   That function argues "BOTH DIRECTIONS, because either one alone is silent" and then enforces both for
   SURFACES and exactly one for COSTS: a key in neither list fails, and a COST this file names that the
   document does not carry fails nothing. An exclusion standing over nothing is not inert — it is a LICENCE:
   the day result.c emits that name again, whatever it then means, this gate drops it from every comparison
   without a word, which is the silence the SURFACES half exists to end. It is also where the paragraphs inside
   DROP[""] rot, since each dropped name carries an argument for why it legitimately differs and an argument
   about a field nobody emits is a stale claim sitting inside the list a reader consults to classify the next
   one.
   IT IS ASKED ONCE PER RUN AND NOT PER (DOCUMENT, SCHEDULE), and it does NOT suppress the comparison. The
   disagreement is between this file's list and the program, so it is the same fact on all seven schedules of
   all seven documents — reporting it fifty times says nothing new — and the run it was found in is still a
   valid measurement, because a name the document does not carry was being dropped from every comparison
   anyway. Suppressing the invariance result over it would trade a real cross-schedule answer for a
   classification note, which is the three-states-behind-one-answer shape this file argues about everywhere.
   AND THE THREE STATES ARE KEPT THREE. `result.c stopped emitting this` and `the artifact predates the field`
   ask for OPPOSITE work — delete a row, or rebuild — and the two are indistinguishable from the document
   alone. MEASURED, WHICH IS WHY THIS IS WRITTEN THIS WAY RATHER THAN AS A FLAT FAILURE: the first run of this
   check reported `pageErrorsExplored` as a stale exclusion on every schedule of flag_fork.html and told the
   reader to delete the row — and `pageErrorsExplored` is in result.c's single `composef` literal at
   origin/main. The artifact was a build stamped f6cbdd9b, 338 commits behind, that predates the field. The
   check was right that the two disagree and its remedy was the wrong one of the two, which is exactly the
   false attribution the `@ENGINEFAIL` token above exists to stop. */
let staleAsked = false;
function reportStaleExclusions(result) {
  if (staleAsked) return 0;
  staleAsked = true;
  const stale = [...DROP.get("")].filter((k) => !(k in result));
  if (!stale.length) return 0;
  if (!ARTIFACT_IS_THIS_REVISION) {
    console.log(`  UNDECIDED  this gate names ${stale.join(", ")} as a schedule-dependent COST and the result ` +
                "document does not carry it — and the artifact that answered is NOT a build of this revision " +
                "(see the [rev] block), so the two things that disagree were not made from one tree. Either " +
                "result.c stopped emitting the field and the row in DROP[\"\"] is a licence standing over " +
                "nothing, or the artifact simply predates the field. Those ask for opposite work — delete the " +
                "row, or `node engine/build.mjs` — and this run cannot tell them apart. Rebuild and re-run to " +
                "get an answer. NOT counted as a failure: the name was dropped from every comparison anyway, " +
                "so the invariance result below is unaffected.");
    return 0;
  }
  console.log(`  FAILED  this gate names ${stale.join(", ")} as a schedule-dependent COST and drops it from ` +
              "every comparison, and the artifact — which IS a build of this revision — does not emit it. The " +
              "exclusion stands over nothing, so the day that name comes back, whatever it then means, it is " +
              "silently excluded. Delete the row from DROP[\"\"] in engine/solvergate.mjs together with the " +
              "paragraph arguing about a field nobody emits, or say what now carries the number.");
  return 1;
}

const arg = process.argv[2] || "";
const docs = readdirSync(CORPUS).filter((f) => f.endsWith(".html")).sort()
                                .filter((f) => !arg || f === arg || f === arg + ".html");
if (!docs.length) {
  console.error(`[solvergate] no document matches ${arg || "*"} in ${CORPUS}`);
  process.exit(1);
}

/* THE BUDGET IS CPU, AND ITS VERDICT IS ABOUT THE DOCUMENT — never about the solver. §Testing: measure the
   thing the invariant is about, and a wall clock is the usual way in; four separate checks in this tree
   reported an artifact of HOW they ran as a defect in WHAT ran. A corpus document for this gate must DRAIN
   (the invariant is over a FINISHED finding set; a frontier that never empties has no such set at any
   schedule), and RLIMIT_CPU is what decides that without ever being falsifiable by a loaded box: a run starved
   by other work is never killed for waiting. A document that exceeds it is reported as a CORPUS DEFECT naming
   the document — the response is to establish why it does not drain, never to raise this number, which would
   be choosing the report over the forcing function.
   The wall backstop is the BACKSTOP for what CPU cannot see — a child deadlocked on nothing, consuming no CPU
   — and reports through a DIFFERENT SIGNAL (SIGTERM from node's timeout, against the kernel's SIGXCPU) so the
   two verdicts never collapse into one. The CPU the killed child actually consumed is printed with it, because
   that number is what separates "starved on a loaded box" from "asleep waiting for something that never
   came". */
const CPU_BUDGET_S = 120;
const WALL_BACKSTOP_MS = 900_000;
/* THE METER IS engine/gate_cpu.mjs's, because the hand-copied one that stood here indexed /proc/self/stat TWO
   FIELDS TOO LOW — `utime`/`stime`, THIS process's own CPU — under a comment that correctly named fields 16
   and 17. It reported the parent's idle time in `waitpid` as the killed child's consumption, which is ~0 by
   construction, so the paragraph above was true of a number that was about the wrong process. */

function runChild(doc, sched) {
  const cpu0 = childCpuSeconds();
  const r = spawnSync("/bin/sh",
    ["-c", `ulimit -H -t ${CPU_BUDGET_S + 10} 2>/dev/null; ulimit -S -t ${CPU_BUDGET_S}; exec "$@"`, "sh",
     process.execPath, SELF, "--run", join(CORPUS, doc), sched],
    { encoding: "utf8", maxBuffer: 1 << 28, timeout: WALL_BACKSTOP_MS });
  const cpuUsed = childCpuDelta(cpu0, childCpuSeconds());
  const out = (r.stdout || "") + (r.stderr || "");
  const emit = out.match(/^@EMIT (\{.*\})$/m);
  if (emit) {
    const e = JSON.parse(emit[1]);
    return { ok: true, out, result: e.result, partial: e.partial, snapshots: e.snapshots };
  }

  /* WHICH OF THE SIX THIS IS, said out loud — the same distinction engine/wpt.mjs draws, and for the same
     reason: a DCHECK naming a missing capability, this gate's own corpus refusal, an ENGINE render path that
     disagreed with itself, an ABI this artifact does not have, a genuine cost, and a fact about the box are
     six different pieces of work, and a message naming them all distinguishes none.
     THE SIXTH IS FIRST BECAUSE IT IS THE ONE THAT WOULD OTHERWISE BE MISREAD RATHER THAN MISSED. Every
     `@GATEFAIL` below is rendered `CORPUS:` and every one of the three refusals that raises it means "move or
     rewrite this fixture"; a `qjs_emit_partial` that answers two different documents at one boundary means the
     fixture is fine and the engine's compose has a side effect, so borrowing the corpus sentence for it sends
     the reader to the one place the defect is not. Matched ahead of `@GATEFAIL` and on a different word, so
     the two cannot collide.
     AND IT CAPTURES TO THE END OF THE OUTPUT, not to the end of a line: `engineFail` exits the moment it has
     printed, so nothing of this run follows it, and the evidence it carries — the diverging documents, or the
     owed list on both sides of the call — is exactly the multi-line part a first-line match would throw away
     at the one moment the reader needs it. */
  const ef = out.match(/^@ENGINEFAIL ([\s\S]*)$/m);
  const gf = out.match(/^@GATEFAIL (.*)$/m);
  const why = out.match(/@WHY .*"reason":"([^"]*)/) || out.match(/^@WHY (.+)$/m);
  /* THE ARTIFACT DOES NOT HAVE THE ABI THIS DRIVER CALLS, and it is its own cause because it is its own piece
     of work — a REBUILD — and because it arrives looking exactly like the thing this gate is for. This is not
     hypothetical: `qjs_init` grew Permissions Policy §9.5's container argument, this driver was updated with
     it, a lane then rebuilt the SHARED artifact from an older checkout, and every document under every
     schedule died in emscripten's own export wrapper before a single flow ran. The revision block below
     already says the artifact is not a build of this revision, but it says it ONCE at the end while
     twenty-eight runs each reported `the child exited 1 with no @EMIT line and no @WHY` — which is the
     three-states-behind-one-answer defect this file argues about @S candidates, performed by its own reporter.
     Matched on emscripten's message because that is where the fact is: the wrapper knows the arity the wasm
     exports and this driver knows the arity it passed, and nothing else in the run compares the two. */
  const abi = out.match(/native function `([a-z_0-9]+)` called with (\d+) args but expects (\d+)/);
  const cause = ef ? "ENGINE: " + ef[1]
              : gf ? "CORPUS: " + gf[1]
              : why ? "DCHECK: " + why[1]
              : abi
                ? `BUILD: the artifact does not export the ABI this driver calls — \`${abi[1]}\` takes ` +
                  `${abi[3]} arguments in extension/lib/qjs and this gate passed ${abi[2]}. Nothing about the ` +
                  "solver was measured: the run died in emscripten's export wrapper before qjs_begin. The " +
                  "artifact is a build of some OTHER revision (see the [rev] block) and the fix is to rebuild " +
                  "it — `node engine/build.mjs` — never to reshape the call to match what happens to be built"
              /* `SIGXCPU || SIGKILL` IS A COLLAPSE, AND IT ASSERTED THE WRONG ONE OF THE TWO. SIGXCPU is the
                 kernel naming the SOFT rlimit THIS driver installed and is a fact about the document; SIGKILL
                 is the OOM killer's usual signal and is a fact about the box, except at the HARD rlimit ten
                 seconds above, where it is this driver's again. Reading them as one sentence told the reader
                 "this document did not DRAIN — find out why" about a run the kernel had killed for memory,
                 which is a confident false claim about the corpus. engine/wpt.mjs already draws this line; the
                 discriminator is the CPU consumed, and the CPU consumed is a number this driver has. */
              : (r.signal === "SIGXCPU" && (cpuUsed === null || cpuUsed >= CPU_BUDGET_S - 1)) ||
                (r.signal === "SIGKILL" && cpuUsed !== null && cpuUsed >= CPU_BUDGET_S)
                ? `this document did not DRAIN inside ${CPU_BUDGET_S}s of CPU actually consumed ` +
                  `(${cpuText(cpuUsed)}, killed by ${r.signal}). That is a ` +
                  "statement about the document, not about the box and not about the solver: the invariant " +
                  "this gate measures is over a FINISHED finding set, and a frontier that never empties has " +
                  "none at any schedule. Find out why it does not drain — do not raise the budget"
              /* THE METER CONTRADICTING THE KERNEL IS THIS DRIVER'S DEFECT, NOT THE DOCUMENT'S. SIGXCPU arrives
                 only after the child has spent the soft rlimit, so a reading materially below it cannot be
                 true — and the arm above must not absorb it into a sentence about the corpus. */
              : r.signal === "SIGXCPU"
                ? `the kernel raised SIGXCPU, which it does only when this child spent the ${CPU_BUDGET_S}s ` +
                  `SOFT rlimit this driver installed — and this driver's CPU meter read ${cpuText(cpuUsed, 2)} ` +
                  "for it. Both cannot be true of one process, the signal is the kernel's and the number is " +
                  "derived, so the number is wrong: engine/gate_cpu.mjs is where it is read. Nothing about " +
                  "this document was measured"
              : r.signal === "SIGKILL"
                ? `SIGKILL after ${cpuText(cpuUsed, 2)} of CPU. ` + (cpuUsed === null
                    ? `Without the meter this driver cannot tell its OWN ${CPU_BUDGET_S + 10}s HARD rlimit ` +
                      "from a kill that came from outside the run, and those ask for opposite work"
                    : `That is far below the ${CPU_BUDGET_S}s budget, so it is NOT a limit this driver ` +
                      "installed — something outside this run killed the process, and the OOM killer is the " +
                      "usual one (dmesg names it)")
              : r.signal === "SIGTERM"
                /* AND THE THIRD STATE THE OLD METER COULD ONLY EVER REPORT AS THE FIRST. Block and starvation
                   are told apart by this one number, so a host that cannot produce it produces NEITHER verdict
                   — where the broken reader produced a 0.0, which is the block arm's entire evidence, on every
                   run on every host. An absent number and a zero number are different facts. */
                ? (cpuUsed === null
                    ? `wall backstop at ${WALL_BACKSTOP_MS / 1000}s, and the CPU this child consumed is ` +
                      `${cpuText(cpuUsed)} — so this driver cannot say whether it was blocked on something ` +
                      "that never came or starved of the thread. Attach to it (gdb -p, /proc/PID/syscall)"
                    : cpuUsed < 1
                    ? `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuText(cpuUsed, 2)} of ` +
                      "CPU — that is a BLOCK, not load: the process was asleep waiting for something that " +
                      "never came. Attach to it (gdb -p, /proc/PID/syscall) and look at the OTHER end"
                    : `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuText(cpuUsed)} of ` +
                      `CPU, load average ${loadavg()[0].toFixed(1)} on ${cpus().length} cores — starved, not ` +
                      "blocked; RE-RUN THIS DOCUMENT ALONE")
              : `the child exited ${r.status} with no @EMIT line and no @WHY`;
  return { ok: false, out, cause };
}

console.log("\n==================== solver gate (schedule invariance) ====================");
let bad = 0;
/* WHAT THE RUN-TO-RUN CHECK ACTUALLY ANSWERED, PER DOCUMENT — and it is a tally rather than a total because a
   total is the one thing it must not be. `7 documents x 5 schedules + 1 repeat` is a statement about what this
   driver INTENDED, and on the first corpus run of this check it was false of the run that printed it: one
   document's reference aborted, so its repeat never happened, and the tail still read as though seven had been
   asked. That is this file's own three-states-behind-one-answer defect committed by its own reporter — an
   absent verdict is invisible next to a column of `held`, which is precisely the shape "a rung whose ABSENCE
   and whose ZERO read alike" names. Each state is counted where it is decided and printed by name.
   AND `held` IS TWO STATES, for the same reason and found the same way — by running this. A `held` across two
   runs that took the SAME interleaving is a weaker sample than one across two that diverged, and the reading
   that said so was a NOTE printed only in the strong case, so the weak case was silent and the two read alike
   in the column. See the verdict site for what each means and why neither is asserted. */
const detTally = { "held/diverged": 0, "held/undiscriminated": 0, "NONDETERMINISTIC": 0, "NOT-MEASURED": 0,
                   "not reached (the reference itself produced no result)": 0 };
for (const doc of docs) {
  const runs = new Map();
  /* HOW MANY TIMES THE BOUNDARY QUESTION WAS ACTUALLY ASKED, kept beside the run that answered it. It is
     the same distinction `held/diverged` against `held/undiscriminated` draws one column over: a `stream`
     row whose document reached DONE on its first step took the FINAL snapshot and no mid-run one, so it
     exercised the two output paths and exercised nothing about a frontier that is still standing — and
     without this number that row and a row with twenty-five mid-run asks read identically. PRINTED, NEVER
     COMPARED: it is a count of boundaries, which is the schedule's to choose, on `_switches`' own ground. */
  const midrun = new Map();
  let broke = false, docBad = 0;
  for (const sched of SCHEDULES) {
    const r = runChild(doc, sched);
    if (!r.ok) {
      bad++; broke = true;
      console.log(`  FAILED ${doc} [${sched}]\n         ${r.cause}`);
      /* A crash leaves the output that preceded it; a kill leaves an empty tail. Print the tail either way so
         the reader can tell which they are looking at without re-running. */
      const tail = r.out.split(/\r?\n/).filter(Boolean).slice(-8);
      if (tail.length) console.log(tail.map((l) => "           | " + l.slice(0, 160)).join("\n"));
      continue;
    }
    /* THE RESUME IS OBSERVED, not assumed. cold_resume prints `@RESUMED <n>` when it rebuilds a parked
       frontier — the observable CLAUDE.md records as having had a reader and no writer for every session there
       has ever been. A `park` run whose second session rebuilt nothing would explore the document from
       scratch and agree with the reference for the wrong reason, which is the false green this checks for. */
    if (sched === "park") {
      const m = r.out.match(/^@RESUMED (\d+)$/m);
      if (!m || +m[1] === 0) {
        bad++; broke = true;
        console.log(`  FAILED ${doc} [park]\n         the resumed session rebuilt ${m ? m[1] : "no"} flow(s) ` +
                    "from the residue — it therefore agreed with the reference by re-exploring from the " +
                    "baseline, not by resuming, and the round trip this schedule exists to measure never ran");
        continue;
      }
    }
    if (!checkCoverage(doc, sched, r.result)) { bad++; broke = true; continue; }
    /* ─── THE STREAMED DOCUMENT AGAINST THE RETURNED ONE ───────────────────────────────────────────────────
       ASKED OF THE SCHEDULE'S DECLARATION, never of whether a document happens to be there. `partial` present
       under a schedule that declared `false`, or absent under one that declared `true`, is the child and this
       loop disagreeing about which run was made — and the shape that hides it is a truthiness test, which
       reads both as "no streaming happened" and passes. Both directions, for checkCoverage's own reason.
       WHAT THE COMPARISON IS FOR, said once so the row is never read as a second cross-schedule check: these
       two documents are one instant apart with no step between them and are composed by the same
       `result_json`, so they are the same document by construction — and the two PATHS out of it are not the
       same path. `qjs_result` hands back a pointer this driver reads through the ccall binding;
       `qjs_emit_partial` prints a line this driver reads off the print sink, which is the channel
       extension/bridge.js's incremental merge is built on. A
       difference here is that channel, and it is the one failure this gate can see that is not about the
       solver at all — so it says so rather than borrowing the MISMATCH row's sentence about dropped flows. */
    const declaresPartial = POLICY.get(sched).partial;
    if (declaresPartial !== (r.partial !== null && r.partial !== undefined)) {
      bad++; broke = true;
      console.log(`  FAILED ${doc} [${sched}]\n         the schedule declares \`partial: ${declaresPartial}\` ` +
                  `and the run carried ${declaresPartial ? "no" : "a"} streamed document — the child takes one ` +
                  "only where the declaration says to, so the two disagree about which run was made and the " +
                  "row below would compare a document nobody asked for or skip one somebody did");
      continue;
    }
    if (declaresPartial) {
      if (!checkCoverage(doc, sched + " (streamed)", r.partial)) { bad++; broke = true; continue; }
      for (const surface of SURFACES.keys()) {
        const a = surfaceSet(r.result, surface), b = surfaceSet(r.partial, surface);
        const onlyReturned = a.filter((x) => !b.includes(x));
        const onlyStreamed = b.filter((x) => !a.includes(x));
        if (!onlyReturned.length && !onlyStreamed.length) continue;
        bad++; docBad++;
        console.log(`  STREAM-SKEW ${doc} [${sched}]  ${surface}: qjs_result has ${a.length}, the ` +
                    `qjs_emit_partial snapshot taken one instant earlier has ${b.length}` +
                    "\n           Both are solver/result.c's `result_json(g_ctx)` over a TERMINAL frontier with " +
                    "no step between them, so they are the same document and a difference is not a solver " +
                    "disagreement: it is the ENGINE's two output paths, and the streamed one is the channel " +
                    "the product's incremental merge reads (extension/bridge.js `streamPartial`). Whatever " +
                    "is missing here is missing from every mid-run snapshot the extension has ever merged.");
        for (const x of onlyReturned) console.log(`             only in qjs_result: ${x.slice(0, 300)}`);
        for (const x of onlyStreamed) console.log(`             only in the stream: ${x.slice(0, 300)}`);
      }
      /* AND THE RUN IS STILL ENTERED FOR THE CROSS-SCHEDULE COMPARISON, because these are two questions and
         dropping the run would answer the second one by not asking it. `qjs_result`'s document is what every
         other schedule is compared against and it is unaffected by whether the STREAM of it agreed; a skew
         suppressed here would take a real cross-schedule cap out of the table along with it, which is the
         three-states-behind-one-answer shape this file argues about everywhere else. Counted in
         `docBad` so the verdict word cannot read `ok` over it. */
    }
    runs.set(sched, r.result);
    midrun.set(sched, r.snapshots);
  }
  const ref = runs.get(REFERENCE);
  if (!ref) {
    if (!broke) { bad++; console.log(`  FAILED ${doc} — the reference schedule \`${REFERENCE}\` produced no result`); }
    /* AND THE REPEAT BELOW IS NOT REACHED, WHICH IS A FOURTH STATE AND HAS TO BE COUNTED AS ONE. A document
       whose reference aborted has no run for a second one to be compared against, so it gets no `det` column
       at all — and an absent column is exactly as invisible as the zero this file argues about everywhere
       else. Counted here so the tail can say `6 held, 1 not reached` instead of a total that implies seven
       documents were asked a question only six of them were. */
    detTally["not reached (the reference itself produced no result)"]++;
    continue;
  }
  /* ASKED HERE BECAUSE THIS IS THE FIRST POINT A RESULT DOCUMENT EXISTS, and asked of the REFERENCE because
     every other document in the run is the same program's `result_json` and would answer identically. It
     returns 0 or 1 and reports itself; the latch inside it is what makes it once per run rather than once per
     document. */
  bad += reportStaleExclusions(ref);
  /* AND A COMPARISON NEEDS A SUBJECT, WHICH IS THE ONE THING THIS GATE HELD EVERY SURFACE TO AND NEVER ASKED
     OF THE DOCUMENT. `checkCoverage` already makes this argument one level down, in these words: a SURFACE
     that is not in the document "is a comparison over nothing, which reads as agreement". A DOCUMENT whose
     accumulating surfaces are all empty is the same sentence one level up — every schedule, the repeat
     included, compares `[]` against `[]`, every row agrees, and the line prints `ok … @H 0 @S 0`. A solver
     that stopped emitting entirely would pass this gate on every document at once, which is the exact false
     green §Testing means by "an absent count and a zero count are DIFFERENT facts … a run that reported no
     document is not a page that was analysed and found clean".
     IT IS NOT AN EXPECTED-EMISSION LIST, and that distinction is the whole reason it is allowed to exist here.
     It states no count, no surface and no finding: a solver that learns twice as much passes it unchanged and
     a solver that learns one thing passes it too, so there is nothing for a better solver to contradict and
     nothing for anyone to maintain. What it refuses is a run with no subject.
     ASKED OF THE ACCUMULATING SURFACES ONLY, on the park pre-check's ground: `_wfq` and `_quantum` are
     readings the terminal instant answers whatever the run learned, so a document could satisfy this on two
     constants alone and the check would be measuring its own denominator.
     ASKED OF THE REFERENCE AND ONCE PER DOCUMENT: a schedule that emitted nothing while the reference emitted
     something is already a MISMATCH row with the findings named in it, and asking this per schedule would
     report that same defect twice under two different sentences. */
  if (!SURFACES_ACCUMULATING.some((s) => surfaceSet(ref, s).length !== 0)) {
    bad++; docBad++;
    console.log(`  FAILED ${doc} — the reference run emitted NOTHING on any of ${SURFACES_ACCUMULATING.join(", ")}, ` +
                "so every comparison this gate makes for this document is `[]` against `[]` and cannot fail. " +
                "That is not a document that was explored and found clean: it is a document with no subject, " +
                "and it would read as `ok` while measuring nothing. Either the corpus entry no longer reaches " +
                "the code it was written for, or the solver stopped emitting — and the second is what this " +
                "gate exists to see, so establish which before touching the fixture.");
  }
  /* ─── THE REFERENCE, ASKED A SECOND TIME ──────────────────────────────────────────────────────────────────
     See the block beside REFERENCE for why this is not a fifth schedule, why it is the reference that is
     repeated and why it runs here. THE THREE STATES ARE KEPT THREE — §Testing: an absent count and a zero count
     are different facts, so a repeat that never ran is NOT a determinism that held, and the summary line below
     carries whichever of the three this document reached. */
  let refNoisy = 0, det = "held", repeatCost = "";
  {
    const again = runChild(doc, REFERENCE);
    if (!again.ok) {
      bad++; det = "NOT-MEASURED";
      console.log(`  FAILED ${doc} [${REFERENCE} x2]\n         the REPEAT of the reference produced no result, ` +
                  "so run-to-run determinism was NOT MEASURED for this document — every comparison below it " +
                  "rests on a baseline nothing has checked, which is a different fact from a baseline that was " +
                  `checked and held. Cause: ${again.cause}`);
      const tail = again.out.split(/\r?\n/).filter(Boolean).slice(-8);
      if (tail.length) console.log(tail.map((l) => "           | " + l.slice(0, 160)).join("\n"));
    } else if (!checkCoverage(doc, REFERENCE + " x2", again.result)) {
      bad++; det = "NOT-MEASURED";
    } else {
      repeatCost = `${policyLabel(REFERENCE)}x2:${again.result._switches}sw/${again.result._flows}fl`;
      for (const surface of SURFACES.keys()) {
        const a = surfaceSet(ref, surface), b = surfaceSet(again.result, surface);
        const only1 = a.filter((x) => !b.includes(x));
        const only2 = b.filter((x) => !a.includes(x));
        if (!only1.length && !only2.length) continue;
        refNoisy++; bad++; det = "NONDETERMINISTIC";
        console.log(`  NONDETERMINISTIC ${doc}  ${surface}: run 1 has ${a.length}, run 2 has ${b.length} — the ` +
                    `SAME document under the SAME schedule \`${REFERENCE}\`, twice, on one build.` +
                    "\n           This is the gate's PREMISE failing and not a schedule effect. §Testing's " +
                    "\"the finding set is a function of the DOCUMENT ALONE\" is false of this build, so every " +
                    "MISMATCH row for this document is a difference between two draws of a noisy process and a " +
                    "real cross-schedule cap is indistinguishable from it." +
                    "\n           IT IS NOT EXPLAINED BY THE CLOCK. The WFQ is LICENSED to reorder on consumed " +
                    "time (flow_silence_notch divides it by one cooperative quantum) and FORBIDDEN to change " +
                    "what a DRAINING frontier emits — so a differing set here is a flow dropped, starved, " +
                    "skipped, reordered or forgotten, with no second schedule involved. Report the two sets " +
                    "before patching anything: this is the first evidence the premise can fail.");
        for (const x of only1) console.log(`             only in run 1: ${x.slice(0, 300)}`);
        for (const x of only2) console.log(`             only in run 2: ${x.slice(0, 300)}`);
      }
      /* WHETHER THE SAMPLE DISCRIMINATED IS PART OF THE VERDICT, NOT A NOTE BESIDE IT — and this was a NOTE
         when it first landed, which made it the same defect as the one directly above it in this file's own
         history. A `held` whose two runs took the same interleaving and a `held` whose two runs took different
         ones are different amounts of evidence, and the note fired only on the second: SILENCE on the weak case
         is exactly what makes the two read alike in the column a reader scans. Measured on the first corpus run
         of this check, on an idle box: every repeat reproduced the reference's switch count exactly, so all six
         `held`s were the weak kind and nothing said so. A verdict that cannot distinguish its own strength is
         the ritual §Testing warns a green becomes.
         WHAT THE TWO MEAN, precisely, because the weaker one is easy to overclaim in the other direction too.
         `held/diverged` — the two runs differ in switch count, so they demonstrably executed different
         interleavings and emitted the same set: the invariant tested against the thing that moves it.
         `held/undiscriminated` — the counts agree, so this sample offers NO EVIDENCE the two runs interleaved
         differently. It is not a failure and not a defect in the document: on an idle box the reference simply
         reproduces itself, which is a property of the BOX (quantum_thread_us is a wall clock on this host, so
         the schedule only moves when something else is competing for it). It is a weaker sample, and the honest
         thing is to say which was taken rather than to report both as one word.
         AND IT IS NOT ASSERTED IN EITHER DIRECTION. Equal counts do not prove equal orders (two orders can
         coincide in count), and unequal counts are not a demand — requiring divergence would make a quiet box a
         gate failure, which is a verdict a loaded machine could falsify, the one shape §Testing forbids
         outright. Both are readings. */
      if (det === "held")
        det = again.result._switches !== ref._switches ? "held/diverged" : "held/undiscriminated";
      if (det === "held/diverged")
        console.log(`         determinism held ACROSS A DIFFERENT INTERLEAVING: ${ref._switches}sw then ` +
                    `${again.result._switches}sw over the same pair — the two runs diverged in execution and ` +
                    "still emitted the same set, which is the invariant tested against the thing that moves it.");
    }
  }
  detTally[det]++;
  let doc_bad = 0;
  for (const [sched, result] of runs) {
    if (sched === REFERENCE) continue;
    /* THE ROWS ARE COLLECTED BEFORE ANY IS PRINTED, because the sentence each one carries depends on a
       measurement that is made ONCE PER SCHEDULE and not once per surface. Printing inside the surface loop is
       what forced the old wording to be chosen before the evidence for it existed. */
    const rows = [];
    for (const surface of SURFACES.keys()) {
      const a = surfaceSet(ref, surface), b = surfaceSet(result, surface);
      const onlyRef = a.filter((x) => !b.includes(x));
      const onlyThis = b.filter((x) => !a.includes(x));
      if (onlyRef.length || onlyThis.length) rows.push({ surface, a, b, onlyRef, onlyThis });
    }
    if (!rows.length) continue;

    /* ─── THE DISAGREEING SCHEDULE, ASKED A SECOND TIME ────────────────────────────────────────────────────
       THE REFERENCE IS REPEATED AND EVERY OTHER SCHEDULE WAS NOT, so a MISMATCH row had exactly one sample on
       each side and stated a two-schedule diagnosis over it. That is the block beside REFERENCE's own argument
       — "if two runs of the reference can differ, then every MISMATCH row is two draws of a noisy process
       reported as a schedule effect" — applied to the reference alone, while the row it protects names TWO
       schedules and the other one had never been asked twice. `refNoisy` closes the half where the BASELINE
       moves; this closes the half where the OTHER SIDE does, and until it existed those two failures printed
       the same sentence.
       THE THREE STATES ARE KEPT THREE, on §Testing's ground that an absent measurement is not a measurement
       that held: REPRODUCED (the schedule agrees with itself and still differs from the reference — a
       cross-schedule effect, which is the cap the row has always claimed), SELF-INCONSISTENT (the schedule
       disagrees with ITSELF, so the finding set is not a function of the document under a FIXED schedule, which
       is a STRONGER violation of the same razor and a different investigation), and NOT MEASURED (the repeat
       produced no result). A row that cannot tell the first from the second sends the reader to compare two
       schedules when the defect is inside one of them.
       IT RUNS ONLY ON A DOCUMENT THAT ALREADY FAILED, so it costs nothing on a green corpus: at most one extra
       child per disagreeing schedule, taken at the point where the gate is already reporting a defect and the
       question "which defect" is the one thing worth another 25 seconds.
       MEASURED, AND THIS IS WHY IT IS HERE RATHER THAN AN IMPROVEMENT IN PRINCIPLE: hash_sink.html produced a
       `direct`/`lastreply` securitySinks MISMATCH in ONE of three gate runs against the build stamped f6cbdd9b,
       and in the failing run `lastreply` carried 3 flows where all the passing runs carried 4 — while three
       direct child runs of `lastreply` on that document all reached the sink. One sample per schedule cannot
       tell that from a schedule effect, and the row it printed named two schedules for a difference the
       evidence puts inside one. */
    const retry = runChild(doc, sched);
    let selfNote;
    if (!retry.ok) {
      selfNote = "           AND THE DISAGREEING SCHEDULE WAS NOT RE-MEASURED: its repeat produced no result " +
                 `(${retry.cause}), so whether it agrees with ITSELF is unknown and the sentence above is the ` +
                 "row's weakest reading rather than its established one.";
    } else {
      const selfDiff = [...SURFACES.keys()].filter((s) => {
        const x = surfaceSet(result, s), y = surfaceSet(retry.result, s);
        return x.some((v) => !y.includes(v)) || y.some((v) => !x.includes(v));
      });
      selfNote = selfDiff.length
        ? `           AND \`${sched}\` DISAGREES WITH ITSELF — a second run of the SAME schedule on the SAME ` +
          `document differs from the first on ${selfDiff.join(", ")} (${result._flows} flows then ` +
          `${retry.result._flows}). So this is NOT a difference between two schedules: the finding set is not ` +
          "a function of the document under a FIXED schedule, which is the same razor violated harder, and the " +
          "two-schedule sentence above is the wrong investigation. Start from this schedule alone."
        : `           AND \`${sched}\` AGREES WITH ITSELF — a second run of it emitted the same set on every ` +
          `surface (${result._flows} flows then ${retry.result._flows}), so the disagreement with the ` +
          "reference REPRODUCED and the two-schedule reading above is the measured one, not the assumed one.";
    }

    for (const { surface, a, b, onlyRef, onlyThis } of rows) {
      doc_bad++; bad++;
      console.log(`  MISMATCH ${doc}  ${surface}: \`${REFERENCE}\` has ${a.length}, \`${sched}\` has ${b.length}` +
                  "\n           The finding set is a function of the DOCUMENT alone (CLAUDE.md §scheduler: only " +
                  "WHICH flow runs next is value-reranked), so a difference here is a flow that one of these " +
                  "two schedules dropped, starved, skipped, reordered or forgot — which is the definition of " +
                  "a cap — or a switched-in flow reading another flow's state.");
      console.log(selfNote);
      /* …UNLESS THE BASELINE IS NOT A MEASUREMENT, in which case the sentence above is a claim this run cannot
         support. A reference that disagrees with ITSELF makes every row against it a difference between two
         draws, so the row is still printed — it is real data — and it is labelled rather than suppressed:
         suppressing it would hide a genuine cross-schedule cap behind a determinism failure, which is the same
         three-states-behind-one-answer defect in the other direction. */
      if (refNoisy)
        console.log("           AND THE BASELINE OF THIS COMPARISON IS NOT A MEASUREMENT: the reference " +
                    "disagreed with ITSELF on this document (the NONDETERMINISTIC row above), so this row " +
                    "cannot be read as a schedule effect until that is fixed.");
      for (const x of onlyRef) console.log(`             only in ${REFERENCE}: ${x.slice(0, 300)}`);
      for (const x of onlyThis) console.log(`             only in ${sched}:  ${x.slice(0, 300)}`);
    }
  }
  /* THE MEASUREMENT, and it is NOT a target. How much the document yielded and what each schedule COST are
     reported because a reader wants to know the gate ran on something real — but a smaller number here is not
     automatically worse (CLAUDE.md §Testing), and nothing in this file compares them. `switches` differing
     between schedules is the point of the exercise, not a defect.
     IT IS ALSO THE ONE NUMBER THAT SAYS WHETHER A SCHEDULE INTERLEAVED AT ALL, which is why it is printed with
     the PAIR that produced it rather than with the schedule's name alone. A schedule at 2 switches over 2 flows
     ran them one after the other; the same document at 13 ran them against each other. Both are legitimate
     orders and the findings must agree across both — but a reader who cannot see which is which will read a
     column of agreements as more pressure than it is, and that is exactly how the axis this file just corrected
     came to be described as the strongest one in the set while delivering the weakest order in it. Printed,
     never compared: the moment this number becomes something to maximise it is a metric used as a target. */
  const cost = [...runs.entries()]
    .map(([s, r]) => `${policyLabel(s)}:${r._switches}sw/${r._flows}fl` +
                     (POLICY.get(s).partial ? `/${midrun.get(s)}snap` : "")).join("  ");
  console.log(`  ${doc_bad || docBad ? "FAIL" : "ok  "} ${doc.padEnd(24)} ` +
              `@H ${String(ref.fetchCallSites.length).padStart(3)}  ` +
              `@S ${String(ref.securitySinks.length).padStart(3)}  ` +
              `err ${String(ref.pageErrors.length).padStart(2)}  det ${det.padEnd(20)} ` +
              `${cost}${repeatCost ? "  " + repeatCost : ""}`);
  /* THE PAGE'S OWN ERRORS, PRINTED. A page error is the forcing function naming an unbuilt capability
     (result.h), and a corpus document that throws is exploring less than it looks like it is — the gate would
     still be measuring invariance, over a run that stopped early. Never a failure here (the two halves are
     different questions), always visible. */
  for (const e of ref.pageErrors) console.log(`         page error: ${e.slice(0, 160)}`);
}
console.log(`  ---- ${docs.length} document(s) x ${SCHEDULES.length} schedules`);
/* THE RUN-TO-RUN LINE IS THE TALLY AND NOT THE INTENTION — see detTally. A `held` here is the invariant not
   falsified by a second sample; it is never the invariant established, because no number of samples does that.
   The switch counts on each row are what say whether a sample discriminated at all. */
console.log("  ---- run-to-run repeat of `" + REFERENCE + "` (SAME document, SAME schedule, twice): " +
            Object.entries(detTally).filter(([, n]) => n).map(([k, n]) => `${n} ${k}`).join(", "));
/* THE REVISION IN THE TAIL, because the tail is what gets pasted — see engine/gate_revision.mjs. */
for (const l of revisionLines(REV_AT_START)) console.log(l);
{
  const moved = revisionMoved(REV_AT_START);
  console.log(moved ? `[rev] AND IT MOVED WHILE THIS RAN — ${moved}`
                    : "[rev] the engine did not move during this run");
}
console.log("==========================================================================");
process.exit(bad ? 1 : 0);
