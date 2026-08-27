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
 * `_routedTasksTargetGone`, `_routedTasksThrew`), and a
 * parked search's `tried` and `turns`. `_switches` exists precisely BECAUSE it differs between an
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

const ENGINE = dirname(fileURLToPath(import.meta.url));
const SELF = fileURLToPath(import.meta.url);
const CORPUS = join(ENGINE, "tests", "solver");
/* THE SHIPPED ARTIFACT, and deliberately not a runner of this gate's own. engine/wpt.mjs builds its own native
   runner because it runs 800 files and an eight-minute wasm link per iteration is a gate nobody runs; this one
   runs a handful of documents through the ENTRY THE EXTENSION LOADS, which costs one import and makes the gate
   a second driver of the production ABI besides engine/route.mjs. §Testing: the shipped entry is the one that
   rots, and a gate that exercises it is worth more than one that exercises a fixture beside it. */
const WASM = join(ENGINE, "..", "extension", "lib", "qjs", "qjs.mjs");

/* THE SCHEDULES. Each is a HOST POLICY — every knob here is one the production host already has and uses, so
   none of them is a test hook grown into the engine for this gate's benefit. Three axes, because the
   invariance has to survive all three and each one moves a different mechanism:
     `direct`   — the reference. Default yield floor (-inf: run on), every owed reply answered as it appears.
     `preempt`  — qjs_set_yield_floor(+Infinity), the host's Level-1 VALUE yield with an unbeatable rival. The
                  running flow is outranked after EVERY flow step, so the engine context-switches maximally:
                  every COW delta swap, every decide_suspend/decide_resume and every concolic pin swap in the
                  document is exercised, thousands of times instead of a handful. This is the schedule that
                  catches a flow reading state that belongs to another one.
     `lastreply`— replies answered ONE PER STEP and in reverse arrival order. This does not change when a flow
                  yields, it changes WHICH flow becomes runnable first — so the fork tree is built in a
                  different order. §Learning-from-replies says the reply is the same reply whenever it lands.
     `park`     — the frontier is PARKED at the first step boundary (qjs_request_park), the residue is carried
                  through the result document exactly as extension/bridge.js carries it to IndexedDB, and a
                  SECOND INSTANCE resumes from it. §Time-travel-resume's whole claim, end to end, in one
                  process. See the child's own comment for why the park is taken before the first pick and what
                  the deeper park still needs.
   `direct` is the reference because it is what the extension does when no other document is competing. */
const SCHEDULES = ["direct", "preempt", "lastreply", "park"];
const REFERENCE = "direct";

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

async function child(docPath, schedName) {
  const html = readFileSync(docPath, "utf8");
  const name = basename(docPath);
  const url = "https://gate.test/app/" + name;
  const factory = await import(WASM);
  const boot = factory.default ?? factory;

  /* The instance helper is engine/route.mjs's, spelled the same way, because it is the same ABI: allocate the
     C string, ccall, read the answer back as one. */
  async function instance() {
    const M = await boot();
    const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
    const str = (f, ...a) => String(M.ccall(f, "string", a.map(() => "number"), a.map(cs)) ?? "");
    /* §2.2.5's BODY, INTO THE INSTANCE'S LINEAR MEMORY — beside the record's JSON, never inside it. JSON
       cannot say a byte sequence, and each way of making it able to is an algorithm run by the zone that
       FETCHED: that is what Fetch §5.2's `text()` was doing in safe-fetch.js, and it is why HTML §8.1.4.2's
       classic-script decode had never once seen the bytes whose charset it exists to honour. The mock body is
       written as source text here, so this is an ENCODE. */
    /* THE REQUEST THIS ANSWERS IS THE PAIR the engine listed — `qjs_pending` answers `METHOD<TAB>URL` lines and
       the reply is delivered against both halves, so a GET and a POST to one address are two questions here. */
    const provide = (method, u, reply, body) => {
      const b = new TextEncoder().encode(body);
      const p = M._malloc(b.length + 1);
      M.HEAPU8.set(b, p);
      try { M.ccall("qjs_provide", "void", ["number", "number", "number", "number", "number"],
                    [cs(method), cs(u), cs(JSON.stringify(reply)), p, b.length]); }
      finally { M._free(p); }
    };
    return { M, cs, str, provide };
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
  function service(e, sched) {
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
    /* ONE PER STEP, LAST FIRST — the whole of the `lastreply` schedule. The list is re-reported every step
       until each entry is filled, so answering the tail each time answers all of them in reverse order. */
    const answer = sched === "lastreply" ? pending.slice(-1) : pending;
    for (const line of answer) {
      const tab = line.indexOf("\t");
      if (tab <= 0)
        gateFail("a pending line carries no METHOD — qjs_pending answers `METHOD<TAB>URL` lines and the reply " +
                 "is delivered against both halves");
      const method = line.slice(0, tab), u = line.slice(tab + 1);
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
  async function session(recipes, sched) {
    const e = await instance();
    /* THE DOCUMENT CROSSES AS A PAIR — a zero byte is legal in a document, and a `strlen` on this side would
       end the parse at the first one. The fixtures here are source text, so this is an ENCODE. */
    {
      const u8 = new TextEncoder().encode(html);
      const hp = e.M._malloc(u8.length + 1);
      e.M.HEAPU8.set(u8, hp);
      e.M.HEAPU8[hp + u8.length] = 0;
      /* THE EMPTY CSP PAIR IS HTML §7.1.7's INHERITED POLICY CONTAINER, ABSENT: this gate roots each instance
         at a fixture document with no creator, so there is no container to clone and CSP §2.2.2's self-origin
         (this address's origin, which the entry derives) is the right one. The empty pair says that, rather
         than being an argument this driver forgot when the entry grew one.
         THE FOUR AFTER IT ARE §7.1.4's EMBEDDER POLICY of that same container, and they are NOT empty — they
         are the section's own "a new embedder policy", because §7.1.7 gives every container one and there is
         therefore no absence to spell. The two values are §7.1.4's token strings; main.c refuses one that
         names none of the three rather than reading it as the default.
         AND THE SECOND-LAST IS HTML §7.3.1.3's PARENT NAVIGABLE, `u` — this gate's fixture documents are
         rooted with no embedder, so their navigables are top-level traversables. It is the engine's own
         encoding for the absence rather than an empty string, because a navigable either has a parent or is a
         top-level traversable and both are facts a host states.
         AND THE LAST IS Permissions Policy §9.5's CONTAINER, `null` — the same fact one algorithm over, and it
         is stated separately for the reason §7.3.1.3 defines the two links separately: a parent is a
         navigable, a container is the ELEMENT that presents it, and a document can be told about one without
         the other. §9.5 takes "null or an element (container)", and null is what this gate's fixtures are:
         it invents them, so nothing presents them, and §9.7 step 1 then returns "Enabled" for every feature.
         Stating it rather than letting the record default is the whole point — the engine refuses a record
         that states NOTHING for the container, because a silent absence and a stated null are different
         claims and only one of them is this gate's. */
      e.M.ccall("qjs_init", "number",
                ["number", "number", "number", "number", "number", "number", "number", "number",
                 "number", "number", "number", "number", "number", "number"],
                [hp, u8.length, e.cs(url), e.cs(name), e.cs(""), e.cs(url), e.cs(""), e.cs(""),
                 e.cs("unsafe-none"), e.cs(""), e.cs("unsafe-none"), e.cs(""), e.cs("u"), e.cs("null")]);
      e.M._free(hp);
    }
    e.M.ccall("qjs_begin", "void", ["number"], [e.cs(recipes)]);
    if (sched === "preempt")
      /* THE HOST'S OWN LEVEL-1 YIELD, with a rival this engine can never outrank. Nothing is dropped across
         it — engine.c returns YIELD with `g_sess_cur` held and the frontier untouched — which is exactly the
         claim under test: the next step must resume the SAME top flow on the byte-identical frontier. */
      e.M.ccall("qjs_set_yield_floor", "void", ["number"], [Infinity]);
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
    for (;;) {
      const r = e.M.ccall("qjs_step", "number", [], []);
      if (r === 0) break;   /* ENGINE_STEP_DONE — the frontier is empty (or was written out) */
      if (r !== 2 && r !== 3)
        gateFail(`qjs_step answered ${r}, which is none of DONE(0)/YIELD(2)/STALLED(3) — the ABI carries three ` +
                 "codes and this gate branches on all three, so a fourth is a contract that moved under it");
      const paid = service(e, sched);
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
    const json = e.str("qjs_result");
    if (!json) gateFail("qjs_result answered nothing — the result document did not serialize");
    const out = JSON.parse(json);
    e.M.ccall("qjs_teardown", "void", [], []);
    return out;
  }

  let result;
  if (schedName === "park") {
    const first = await session("", "park");
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
       the other. `surfaceSet` answers for either shape, so the emptiness question is asked once. */
    for (const surface of SURFACES.keys())
      if (surfaceSet(first, surface).length !== 0)
        gateFail(`the session parked before its first pick still emitted ${surfaceSet(first, surface).length} ` +
                 `${surface} — this gate compares the RESUMED session against the reference on its own, which ` +
                 "is sound only while the parking session emits nothing. It emitted something, so the two " +
                 "sessions' findings now have to be FOLDED, and that fold is endpoint.c's merge rule (see the " +
                 "park comment above): build it in one place before widening this schedule");
    result = await session(first._park.join(";"), "park");
  } else {
    result = await session("", schedName);
  }
  console.log("@EMIT " + JSON.stringify({ document: name, schedule: schedName, result }));
}

/* ─── canonical form ────────────────────────────────────────────────────────────────────────────────────────
   THE SET, NOT THE SEQUENCE. Emission ORDER is the schedule's to choose — which flow emitted first is exactly
   what a different schedule changes — so every array is sorted by its own canonical text and every object's
   keys are sorted. What survives is the SET of findings, which is what the invariant is about.
   A DROPPED FIELD IS DROPPED BY PATH, so the exclusion is stated about one place in one document rather than
   about a name that might mean something else somewhere else. */
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
                "_routedTasksThrew"])],
  /* `turns` IS A SWITCH-IN COUNT, WHICH IS `_switches` ONE LEVEL DOWN. solve.c counts it in solve_flow_begin —
     the scheduler's every switch-in of a candidate flow — and says so where it counts it: "IT IS SWITCH-INS AND
     NOT DISTINCT FLOWS, which is what makes it a scheduling fact rather than a second copy of `tried`: a
     candidate preempted and resumed twenty times has been given twenty turns". The `preempt` schedule above is
     DEFINED as outranking the running flow after every flow step, so it maximises exactly this number by
     construction; holding it invariant would fail `direct` vs `preempt` on healthy code, which is the one
     ground `_switches` is dropped on and the same ground here. Its NEIGHBOURS stay compared and that is the
     line: `reached`, `survived`/`survivedOf`, `escaped` and `fires` are observations of what a re-execution got
     through the page's own transforms, and on a frontier that DRAINS every seeded candidate runs, so each of
     them converges to a fact about the document. A count of how many times work was switched in never
     converges, because the schedule chooses it. THE ARGUMENT IS BY CONSTRUCTION AND NOT FROM A MEASUREMENT,
     which is stated rather than implied: this is a classification made before the false red rather than after
     one, so nobody should read the row as evidence that a `turns` mismatch was ever observed here. */
  [".securitySinks[]", new Set(["tried", "turns"])],
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
   EACH SURFACE DECLARES ITS SHAPE, because assuming one is how this list went wrong. THE ONLY SHAPE DECLARED
   TODAY IS `array`, and the "map" half of `shapeOk`/`surfaceSet` below has no declarer — said plainly here
   rather than left for a reader to discover, because a mechanism nobody exercises is one nobody knows is
   broken. It was `probeResults`, error-derived request schemas keyed by the `<METHOD> <host><path>` identity
   the Send panel resolves a request body with, and the first run of this gate reported it as an unclassified
   field on all five corpus documents because the coverage check asked `Array.isArray` of a surface that is
   not one. That surface is gone from result.c: an API's rejection is the answer to a DELIBERATELY MALFORMED
   request, which this engine cannot issue, so it is read in the trusted zone (extension/lib/req2proto.js) and
   never crosses the seam this gate compares. Its row is deleted with it rather than left asserting a field no
   writer produces. */
const SURFACES = new Map([
  ["fetchCallSites", "array"],
  ["securitySinks",  "array"],
  ["pageErrors",     "array"],
]);
const shapeOk = (v, kind) => kind === "array" ? Array.isArray(v)
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
  const absent = [...SURFACES].filter(([k, kind]) => !shapeOk(result[k], kind))
                              .map(([k, kind]) => `${k} (${kind})`);
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
  if (SURFACES.get(surface) === "array") return v.map((e) => canonStr(e, "." + surface + "[]"));
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
const CLK_TCK = Number(spawnSync("getconf", ["CLK_TCK"], { encoding: "utf8" }).stdout.trim()) || 0;
if (!CLK_TCK) { console.error("[solvergate] getconf CLK_TCK answered nothing, so a child's CPU cannot be read"); process.exit(1); }
const childCpu = () => {
  const f = readFileSync("/proc/self/stat", "utf8");
  const v = f.slice(f.lastIndexOf(")") + 2).split(" ");   /* comm may hold spaces; fields resume after it */
  return (Number(v[11]) + Number(v[12])) / CLK_TCK;       /* stat(3) fields 16,17: cutime, cstime */
};

function runChild(doc, sched) {
  const cpu0 = childCpu();
  const r = spawnSync("/bin/sh",
    ["-c", `ulimit -H -t ${CPU_BUDGET_S + 10} 2>/dev/null; ulimit -S -t ${CPU_BUDGET_S}; exec "$@"`, "sh",
     process.execPath, SELF, "--run", join(CORPUS, doc), sched],
    { encoding: "utf8", maxBuffer: 1 << 28, timeout: WALL_BACKSTOP_MS });
  const cpuUsed = childCpu() - cpu0;
  const out = (r.stdout || "") + (r.stderr || "");
  const emit = out.match(/^@EMIT (\{.*\})$/m);
  if (emit) return { ok: true, out, result: JSON.parse(emit[1]).result };

  /* WHICH OF THE FIVE THIS IS, said out loud — the same distinction engine/wpt.mjs draws, and for the same
     reason: a DCHECK naming a missing capability, this gate's own corpus refusal, an ABI this artifact does
     not have, a genuine cost, and a fact about the box are five different pieces of work, and a message naming
     them all distinguishes none. */
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
  const cause = gf ? "CORPUS: " + gf[1]
              : why ? "DCHECK: " + why[1]
              : abi
                ? `BUILD: the artifact does not export the ABI this driver calls — \`${abi[1]}\` takes ` +
                  `${abi[3]} arguments in extension/lib/qjs and this gate passed ${abi[2]}. Nothing about the ` +
                  "solver was measured: the run died in emscripten's export wrapper before qjs_begin. The " +
                  "artifact is a build of some OTHER revision (see the [rev] block) and the fix is to rebuild " +
                  "it — `node engine/build.mjs` — never to reshape the call to match what happens to be built"
              : (r.signal === "SIGXCPU" || r.signal === "SIGKILL")
                ? `this document did not DRAIN inside ${CPU_BUDGET_S}s of CPU actually consumed. That is a ` +
                  "statement about the document, not about the box and not about the solver: the invariant " +
                  "this gate measures is over a FINISHED finding set, and a frontier that never empties has " +
                  "none at any schedule. Find out why it does not drain — do not raise the budget"
              : r.signal === "SIGTERM"
                ? (cpuUsed < 1
                    ? `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuUsed.toFixed(2)}s of ` +
                      "CPU — that is a BLOCK, not load: the process was asleep waiting for something that " +
                      "never came. Attach to it (gdb -p, /proc/PID/syscall) and look at the OTHER end"
                    : `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuUsed.toFixed(1)}s of ` +
                      `CPU, load average ${loadavg()[0].toFixed(1)} on ${cpus().length} cores — starved, not ` +
                      "blocked; RE-RUN THIS DOCUMENT ALONE")
              : `the child exited ${r.status} with no @EMIT line and no @WHY`;
  return { ok: false, out, cause };
}

console.log("\n==================== solver gate (schedule invariance) ====================");
let bad = 0;
for (const doc of docs) {
  const runs = new Map();
  let broke = false;
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
    runs.set(sched, r.result);
  }
  const ref = runs.get(REFERENCE);
  if (!ref) {
    if (!broke) { bad++; console.log(`  FAILED ${doc} — the reference schedule \`${REFERENCE}\` produced no result`); }
    continue;
  }
  let doc_bad = 0;
  for (const [sched, result] of runs) {
    if (sched === REFERENCE) continue;
    for (const surface of SURFACES.keys()) {
      const a = surfaceSet(ref, surface), b = surfaceSet(result, surface);
      const onlyRef = a.filter((x) => !b.includes(x));
      const onlyThis = b.filter((x) => !a.includes(x));
      if (!onlyRef.length && !onlyThis.length) continue;
      doc_bad++; bad++;
      console.log(`  MISMATCH ${doc}  ${surface}: \`${REFERENCE}\` has ${a.length}, \`${sched}\` has ${b.length}` +
                  "\n           The finding set is a function of the DOCUMENT alone (CLAUDE.md §scheduler: only " +
                  "WHICH flow runs next is value-reranked), so a difference here is a flow that one of these " +
                  "two schedules dropped, starved, skipped, reordered or forgot — which is the definition of " +
                  "a cap — or a switched-in flow reading another flow's state.");
      for (const x of onlyRef) console.log(`             only in ${REFERENCE}: ${x.slice(0, 300)}`);
      for (const x of onlyThis) console.log(`             only in ${sched}:  ${x.slice(0, 300)}`);
    }
  }
  /* THE MEASUREMENT, and it is NOT a target. How much the document yielded and what each schedule COST are
     reported because a reader wants to know the gate ran on something real — but a smaller number here is not
     automatically worse (CLAUDE.md §Testing), and nothing in this file compares them. `switches` differing
     between schedules is the point of the exercise, not a defect. */
  const cost = [...runs.entries()].map(([s, r]) => `${s}:${r._switches}sw/${r._flows}fl`).join("  ");
  console.log(`  ${doc_bad ? "FAIL" : "ok  "} ${doc.padEnd(24)} ` +
              `@H ${String(ref.fetchCallSites.length).padStart(3)}  ` +
              `@S ${String(ref.securitySinks.length).padStart(3)}  ` +
              `err ${String(ref.pageErrors.length).padStart(2)}   ${cost}`);
  /* THE PAGE'S OWN ERRORS, PRINTED. A page error is the forcing function naming an unbuilt capability
     (result.h), and a corpus document that throws is exploring less than it looks like it is — the gate would
     still be measuring invariance, over a run that stopped early. Never a failure here (the two halves are
     different questions), always visible. */
  for (const e of ref.pageErrors) console.log(`         page error: ${e.slice(0, 160)}`);
}
console.log(`  ---- ${docs.length} document(s) x ${SCHEDULES.length} schedules`);
/* THE REVISION IN THE TAIL, because the tail is what gets pasted — see engine/gate_revision.mjs. */
for (const l of revisionLines(REV_AT_START)) console.log(l);
{
  const moved = revisionMoved(REV_AT_START);
  console.log(moved ? `[rev] AND IT MOVED WHILE THIS RAN — ${moved}`
                    : "[rev] the engine did not move during this run");
}
console.log("==========================================================================");
process.exit(bad ? 1 : 0);
