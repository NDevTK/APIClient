// The scheduler's own order census, SAMPLED ACROSS A LIVE PAGE'S WINDOW.
//
// WHY IT IS A SERIES AND NOT A READING. `solver/result.c` composes `_wfq` and states which of its
// rows may be differenced: `picksLifetime`, `starvedPicks`, `topForgiven`, `workDone` and the scan
// counters are LIFETIME COUNTERS; `picksLive`, `picksMax`, `neverPicked`, `neverPickedAtTop` and
// every `svc*` notch are GAUGES over the members standing NOW and CAN FALL between two samples. A
// single census therefore cannot answer the question anybody asks of it — "is the tail being
// reached" — because that is a claim about a trajectory. bridge.js's `streamPartial` rewrites the
// run's row every 750 ms, so the shipped path already produces the series; nothing here computes it.
//
// SO THIS DRIVER POLLS AND PRINTS ROWS, AND DOES NO ARITHMETIC ON THEM. It deliberately does not
// emit `starvedPicks / picksLifetime`, which is the one number a reader wants: result.c says that
// fraction is read "never raw", and it sums re-dispatches that CONTINUE a framed program — which are
// necessary work — with those that pass over a never-run member, which are the defect. A quotient of
// both is a reading of neither, so printing it here would manufacture the authority the contract
// withholds. The columns are printed side by side and the reader does the division knowing what is
// in it.
//
// WHAT IT CHECKS RATHER THAN ASSUMES. result.c states one identity that is checkable from outside:
// `picksLifetime` must EQUAL `_switches` on the same document, because flow_credit_pick has one
// caller and engine.c raises the switch count beside it. That is printed as `switchDelta` on every
// row — a non-zero value is the census disagreeing with the run record it rides on, which would make
// every other row here a number about nothing.
//
// AND IT SAYS WHICH RUN EACH ROW BELONGS TO. A row is only a sample of the site named on it while
// the scheduler is alive; bridge.js sets `_hostDead` once and never clears it, so the same liveness
// column the other two drivers carry is here too, for the same reason.
//
//   node testing/live-wfq.js <url> [url…]        (one navigation per url, in order)
//
"use strict";

const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");
const { artifactStamp } = require("./artifact_stamp.js");

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const WINDOW = Number(process.env.LIVE_WFQ_WINDOW_MS || 45000);
const EVERY = Number(process.env.LIVE_WFQ_EVERY_MS || 1500);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* THE ROWS THIS DRIVER PRINTS, and the kind of each, taken from result.c's own statements rather
   than from the names — because the names do not say, and result.c records a wrong relay that was
   caused by exactly that (`svcMax` read as a dispatch count when it is a quotient of thread time by
   one cooperative quantum). Only the COUNTER rows may be differenced across samples. */
/* `arrivals`/`departures` are here and not among the gauges because solver/flow.h states their kind: both
   are lifetime counts with exactly one writer each, so both may be differenced — which is what turns
   `arrivals / picksLifetime` (members minted per dispatch) into a RATE over the window this driver samples
   rather than an average over the session. `members` beside them is a gauge and is not the same question.
   Printed, never divided here, for the reason the header gives about `starvedPicks / picksLifetime`. */
/* `starvedPicksIdle` IS NAMED BESIDE `starvedPicks` BECAUSE THE SUPERSET ALONE IS THE HALF result.c SAYS NOT
   TO READ, and this file printed only that half. In result.c's own words: "READ THIS AGAINST `picksLifetime`
   AND NOT `starvedPicks` AGAINST IT.  The superset answers how often the tie-break decided a dispatch; only
   this one answers how often it decided one WRONGLY, which is the question the two opposite repairs hang on."
   The superset sums a framed member re-picked to FINISH its program — necessary work no ordering should
   interrupt on a tie, and the ordinary shape of every quantum of every multi-quantum program on a forking
   page, since an arm is born at its parent's exact weight — with a pass-over of a member that had nothing in
   front of it, which is the defect.  A quotient of both is a reading of neither, so a driver carrying only
   the superset hands a reader the number result.c withholds and withholds the one it prescribes.
   IT HAD NO READER ANYWHERE: `starvedPicksIdle` occurs in the file that raises it and the file that emits it,
   and nowhere else — not build.mjs, not solvergate, not here — so the discriminator between "the ordering is
   passing over starved members" and "the frontier is finishing programs" has never been read on a real page.
   A LIFETIME COUNTER raised at the same line and under the same condition as its superset, so
   `starvedPicksIdle <= starvedPicks` is an identity of one evaluation; it is checked below rather than
   trusted, for the reason every other identity on this stream is. */
/* `epochRebuildLifetime` AND `epochResetsLifetime` ARE COUNTERS AND BOTH READINGS THEY CARRY ARE COMPOSED
   FROM ONE SAMPLE, BELOW.  THIS HEADLINE READ "THE READING THIS DRIVER CANNOT COMPOSE FROM ONE SAMPLE" AND
   IS REWRITTEN RATHER THAN DELETED, because a reader who re-derives it from where the operands SIT in this
   file will write it again: `epochRebuildLifetime` is named in COUNTERS and `scanNextWeights` in COST, so
   they read as two documents and are one.  MEASURED through this driver's own `wfqComposerKeys()` rather
   than argued -- all four epoch rows AND both `scanNext*` rows are keys of the SINGLE composer
   `result_wfq_json(void)`, with an invented name absent from that same set as the control -- so result.c's
   own "READ IT AGAINST `scanNextWeights`, WHICH IS ALREADY ON THIS LINE" is literal, and the retired
   headline was a claim about THIS FILE'S LAYOUT mistaken for one about the census.
   WHAT THE WRONG HEADLINE COST IS WHY IT IS RECORDED AND NOT JUST FIXED: `share`'s contract is that a
   quotient is printed where result.c PRESCRIBES it and nowhere else, and result.c prescribes BOTH of these
   in its own words.  A headline saying this driver could not compose them is therefore not a cautious
   omission -- it is the one sentence standing between a published row and the reading it exists for, which
   is the write-with-no-READING half of the record-field contract and the half no producer-side check can
   see, because the producer is correct.  Both are LIFETIME over LIFETIME, so neither carries the defect
   that makes a lifetime total over a terminal GAUGE under-read by the factor the frontier grew.
   They are the whole maintenance an index over solver/flow.c's `flow_index_key` would pay:
   flow_credit_emit sends a family back to its epoch base by moving a generation with NO per-member write, so
   such an index rebuilds exactly the members standing AWAY, and the first of these is that count summed at
   every emission.  Read it against `scanNextWeights` on the COST scope — the lifetime sum over scans of the
   members each one weighed, which is the walk an index would REPLACE — and against the second of these for
   the average rebuild per emission.  Both are differenceable; the gauges below are NOT the cost and are not
   a smaller version of it.
   A ZERO IS READ AGAINST `epochResetsLifetime` FIRST and is three-state like every other absence here: zero
   resets is a run that never emitted, so the pair is SILENT about the design rather than favourable to it,
   and zero rebuild across non-zero resets is the strongest result available. */
const COUNTERS = ["picksLifetime", "starvedPicks", "starvedPicksIdle", "workDone", "rankChanges",
                  "topForgiven", "arrivals", "departures",
                  "epochRebuildLifetime", "epochResetsLifetime"];
/* `epochAwayLive` AND `epochAwayWalk` ARE GAUGES AND THEY ARE A CONSERVATION IDENTITY RATHER THAN TWO
   READINGS — the first maintained incrementally at four sites in solver/flow.c and summed over distinct
   families, the second that same census asking `flow_own_silence` of every member.  Two maintainers over one
   instant, so a difference is a FIFTH transition site the incremental count does not have.  Checked below
   rather than trusted, for the reason every other identity on this stream is: the engine's own DCHECK is
   compiled OUT of the release build this driver samples.
   THEY ARE NOT THE COST AND MUST NOT BE DIFFERENCED INTO ONE.  A census lands at an arbitrary point between
   two emissions, so this population reads near zero just after one and at its peak just before — one sample
   is a lottery and two of them measure where the samples fell.  `epochAwayLive / members` is worth reading
   as how much of the frontier is off its base right now, and nothing else. */
const GAUGES = ["members", "unrun", "neverPicked", "neverPickedGap", "neverPickedAtTop",
                "epochAwayLive", "epochAwayWalk",
                "picksLive", "picksMax", "families", "jobsReady", "jobsFramed", "jobsOwed",
                /* …AND WHICH ARM OF flow_step CAN DISPATCH THE READY HALF, which `jobsReady` alone cannot
                   say: the checkpoint arm stands above the program sequence and the task arm below it, so
                   an all-TASK backlog is the sequence arm's exclusion MEASURED and any MICROTASK refutes that
                   exclusion for the jobs it counts. It does not say those jobs would have run — arms stand
                   above the checkpoint too. Gauges, like every row on this line. */
                "jobsReadyTask", "jobsReadyMicro",
                /* …AND WHAT A SUB-LINEAR ASK WOULD HAVE TO INDEX, which is the other half of the cost scope
                   below and which no reader in this tree had: `silCarry` occurs in ONE file, result.c, which
                   emits it, and `silPhases` in that file and the header that declares it. Both are the key
                   the ask's own retirement clause names — solver/flow.c: "this pair goes when the ask no
                   longer walks the frontier — at which point the index IS its reader" — so until this line
                   the operands of the proposed index were published and measurable nowhere but a fixture.
                   result.c: `silPhases` is how many DISTINCT remainders the frontier stands on and
                   `silCarry` how many members are on the far side of the carry boundary; between two
                   frontier generations the carry is the ONLY per-member quantity in the order that moves,
                   and members sharing a remainder flip it together. GAUGES, both: `silCarry` FALLS as the
                   boundary sweeps downward and every member resets at once when the family's remainder
                   wraps, and neither may be differenced — which is why they are here and not in COUNTERS. */
                "silPhases", "silCarry",
                "delivReady", "delivFramed", "delivOwed", "valTop", "valMin", "valMax"];

/* THE BRANCH SCOPE, WHICH THE TWO LISTS ABOVE DO NOT REACH AND WHICH IS THE ONE FAMILY A PAGE-SCALE
   SAMPLER EXISTS FOR. solver/result.c publishes twenty-three rows at this scope and says why they are
   worth reading at all: flow_weight sums flow_branch_bonus over the top-level arm's bucket, so these are
   "the only published statement of a TERM OF THE ORDERING". engine/build.mjs reads eleven of them on the
   SMOKE FIXTURE; this file read NONE of them, so the question a branching frontier actually poses — does
   an arm convert fork factor into thread time — had no instrument outside a fixture whose fork factor is
   its author's design decision rather than a document's.

   AND THE MINTER TRIPLE HAD NO READER ANYWHERE. `brMinterLive`, `brMinterGoneLife` and `brMinterUsLife`
   occur in exactly ONE file in this tree — result.c, which emits them — while `brCrowd*` occurs in two.
   That is the write-with-no-reader half of the record-field contract, and here it is not a tidiness
   defect: result.c states that the crowd is selected by `brLiveMax`, a MEMBERSHIP fact, while an arm that
   "forks at every position of an unknown length and lets each arm FINISH mints unboundedly and stands
   narrow, so it owns the mint maximum, is NOT the crowd, and had no live count and no receipt on this
   line at all" — and names that, in its own words, as "the shape the whole aging mechanism was written
   against". `sub_born = live + sub_gone` makes the crowd and the minter ONE bucket only while nothing has
   departed. So a reader holding only the crowd rows is measuring a different arm from the one the aging
   question is about, and cannot tell that it is. Both triples are printed side by side here for exactly
   that reason. */
const RESULT_C = path.join(__dirname, "..", "engine", "host", "solver", "result.c");

/* THE POPULATION IS TAKEN FROM THE COMPOSER, NEVER FROM A LIST BESIDE IT — engine/build.mjs's `wfqFields()`
   rule, owed here for the same reason and in BOTH directions. A row this file names that result.c has
   renamed throws naming the row; a branch-scope row result.c ADDS that this file does not name throws too,
   because the defect this block closes IS a published row with no reader and a one-way check is how it
   recurs. Read at startup so it fails before a window of samples is spent. */
function wfqComposerKeys() {
  const src = fs.readFileSync(RESULT_C, "utf8");
  const i = src.indexOf("char *result_wfq_json(void)");
  if (i < 0) throw new Error("[live-wfq] cannot find `result_wfq_json` in " + RESULT_C + " — this driver " +
                             "takes its row set from that composer, and a census it cannot read is one " +
                             "whose rows it would compare as undefined.");
  const j = src.indexOf("\n}\n", i);
  if (j < 0) throw new Error("[live-wfq] `result_wfq_json` in " + RESULT_C + " has no closing brace at " +
                             "column 0 — the span this reader derives its row set from is unbounded.");
  return new Set([...src.slice(i, j).matchAll(/\\"([A-Za-z_][A-Za-z0-9_]*)\\":/g)].map((m) => m[1]));
}

/* THE KINDS CANNOT BE DERIVED FROM A FORMAT STRING, so they are written here from result.c's own prose
   with the reason, exactly as the COUNTERS/GAUGES split above is. result.c: "The only rows on this line a
   reader may difference are `brUsLifeSum`, `brRetiredUsLife` and `chargedUsLife`, whose population is
   every microsecond ever charged rather than whichever buckets are standing." Everything else at this
   scope is a GAUGE or a per-bucket lifetime read at ONE instant — because THE BUCKET SELECTED MOVES
   BETWEEN SAMPLES, and an extremum over the buckets standing is not its field's kind. Differencing one of
   those across two rows of this stream is arithmetic about two different arms. */
const BR_DIFFABLE = ["brUsLifeSum", "brRetiredUsLife", "chargedUsLife"];
const BR_INSTANT  = ["branches", "brLiveMax", "brLiveMin", "brLiveSum",
                     "brBornLifeMax", "brBornLifeMin",
                     "brCrowdLive", "brCrowdBornLife", "brCrowdUsLife",
                     "brMinterLive", "brMinterGoneLife", "brMinterUsLife",
                     "brUsLifeMax", "brUsLifeMin", "brHeldUsLife", "brEmptyUsLife",
                     "brDepthMax", "brFanMax", "brFanSum", "brFanDepth"];

function branchScope() {
  const keys = wfqComposerKeys();
  const named = [...BR_INSTANT, ...BR_DIFFABLE];
  for (const k of named)
    if (!keys.has(k))
      throw new Error("[live-wfq] result_wfq_json no longer publishes `" + k + "` — this driver names it " +
                      "from result.c's own composer, so a row that has gone is one the producer renamed " +
                      "or dropped rather than one this file invented, and every reading below it would " +
                      "compare an absent field as undefined.");
  const published = [...keys].filter((k) => /^(?:branches|br[A-Z])/.test(k) || k === "chargedUsLife");
  const unread = published.filter((k) => !named.includes(k));
  if (unread.length)
    throw new Error("[live-wfq] result_wfq_json publishes branch-scope row(s) no reader here names: " +
                    unread.join(", ") + ". This check exists because that is precisely how the minter " +
                    "triple came to be emitted by one file and read by none — name the row and say what " +
                    "it means, or the census grows a reader-less column again.");
  return named;
}

/* THE COST SCOPE — WHAT ASKING THIS ORDER COST, which every row in the two lists above is silent about
   because every row above is about what the order DECIDED. result.c publishes nine rows here and states
   what they are FOR in its own words: "the tail is not being reached" has two causes — "not enough thread
   time for the members standing, or the thread spent asking the order rather than running it" — and no
   other row on that line separates them.

   THEY HAD NO LIVE-PAGE READER AT ALL, which is the same defect one scope over from the minter triple and
   is why the check below is written the way `branchScope` is. `engine/build.mjs` reads all nine and
   `engine/solvergate.mjs` reads three, and both of those drive the SMOKE FIXTURE — whose fork factor,
   frontier growth and dispatch count are its author's design decisions rather than a document's. So the
   one question a growing frontier poses about the ORDERING'S OWN COST — does a dispatch weigh a
   constant number of members or all of them — was measurable only where the population was chosen.

   THE KINDS, FROM result.c's OWN TEXT AND NOT FROM THE NAMES. All nine are LIFETIME COUNTS and may be
   differenced: `preemptAsksLifetime` is stated as one outright ("solver/engine.c never resets it, so two
   censuses carrying one `workDone` give a RATE over the interval between them, and that is the only
   reading a wall-denominated quantum leaves quotable at all"), and the eight `scan*` rows are the same
   kind by the same argument — they count WALKS AND WEIGHINGS PERFORMED, which nothing takes back. They
   are COUNTS AND NOT CLOCKS on purpose: solver/flow.h's FLOW_SCANS says a duration here "would be a fact
   about the machine and these are facts about what the engine did", and this host's quantum is
   wall-denominated. */
const COST = ["scanNextRuns", "scanNextWeights", "scanRivalRuns", "scanRivalWeights",
              "scanOtherRuns", "scanOtherWeights", "scanCensusRuns", "scanCensusWeights",
              "preemptAsksLifetime",
              /* …AND WHICH HALF OF THE HOOK'S KEY MOVED WHEN IT MISSED. `scanRivalRuns` is close to half of
                 all the frontier weighing this engine does and is keyed on a DISJUNCTION — the frontier
                 generation OR the incumbent — so until these three the miss rate above had to be read with
                 an assumption about which disjunct supplied it, and the two take opposite diffs. A `gen`
                 miss is the order genuinely having changed; a `cur` miss is a walk for a frontier in which
                 only the EXCLUDED member moved. `rivalMissBoth` is what prices either repair: where both
                 halves moved in one interval, removing one invalidator buys NOTHING because the other would
                 have forced the same walk. LIFETIME counts like the nine above, differenceable for the same
                 reason, and a PARTITION of `scanRivalRuns` — which is checked below rather than assumed. */
              "rivalMissGen", "rivalMissCur", "rivalMissBoth"];

/* …AND WHETHER THE ONE ASSERTION THE DISPATCH WALK MAKES WAS EVER ACTUALLY ASKED, which is the other half of
   the same question and had it WORSE: `keyStaleGenLifetime`, `keyFirstSeenLifetime` and `keyRunningLifetime`
   occur in exactly ONE file in this tree — result.c, which emits them — and `keyArmedLifetime` in that file
   and the one that raises it. No driver anywhere, fixture or live, read any of the four.

   WHY THAT IS THE EXPENSIVE ONE TO HAVE UNREAD. result.c: flow_pick's member-key invariant "is a predicted
   ABSENCE, and a run in which it never fires is satisfied identically by an invariant that HOLDS and by a
   walk that COMPARED NOTHING.  Its condition exempts a member on three arms before it compares anything".
   `keyArmedLifetime` is the SCORE — how many comparisons were actually made — and the other three are the
   exemptions that say where the rest went; result.c states they PARTITION the classifications together.  So
   an engine-side claim of the form "this invariant held on a real page" is UNSCORED without them, and every
   index, heap or cached maximum proposed over this frontier is derived from that invariant.

   ALL FOUR ARE LIFETIME COUNTS and may be differenced.  They are raised under `#if APICLIENT_DEV`, so they
   read 0 in a release build for a reason that is not about the engine — MEASURED that the artifact this
   driver samples is a DEV build, by the presence of that walk's own abort text in the shipped wasm with an
   invented string as the control, so a zero here is a statement about the run and not about the build.  A
   reader meeting four zeros should re-take that control before concluding anything. */
/* …AND THE SECOND PAIR AT THE SAME SCOPE, WHICH SCORES A DIFFERENT CLAIM ABOUT THE SAME KEY. The four above
   score whether the member key STANDS STILL between two frontier generations; `keyIndexAskedLifetime` and
   `keyIndexDifferedLifetime` score whether it ORDERS — whether an index over it would have returned a member
   the comparator also calls maximal. A candidate set rests on the second and nothing measured it.
   READ THEM AS A PAIR. The ask is the reachability witness: a zero `differed` beside a zero ask is a fold
   that never ran, and beside a large ask it is the strongest available result — the surrogate picked the
   SAME member every time. A nonzero `differed` is two members named apart, which is this frontier's ordinary
   state and not a defect.
   THIS READ `the disagreement that matters ABORTS in flow_pick and is on no row here`, AND IS REWRITTEN
   RATHER THAN DELETED BECAUSE THAT IS THE READING A DRIVER RE-DERIVES FROM A ROW THAT ONLY EVER COUNTED
   TIES. That abort is gone — it asked two spellings of one weight to agree bit for bit where flow.c's own
   flow_index_margin declares them a derived distance apart, so it ended every run that reached a few thousand
   members on a real page — and the disagreement it used to end a run on is now the PARTITION below:
   `keyIndexDifferedTieLifetime` and `keyIndexDifferedStrictLifetime`, summing to `keyIndexDifferedLifetime`.
   So a defect that used to be readable from one dying run's crash text is readable from every run's census,
   and a reader who wants to know how OFTEN the surrogate names a worse member has a row instead of a count of
   aborted sessions.
   THE STRICT ARM IS A SHARE AND NEVER A QUANTITY. Large against the differed total, the single top key names
   a member the comparator calls worse repeatedly; small beside a much larger tie count, the order is simply
   tied and the design flow.c proves exact — a candidate set within a derived margin, re-compared through
   flow_weight — answers it with no edit to the order at all.
   They are named here because result.c publishes them and `keyScope`'s derived check would throw otherwise —
   which is that check doing the one thing it exists for. An artifact older than these rows prints null for
   each, which is this stream's absent-versus-zero rule and is the honest answer: the run did not state them. */
/* …AND THE THIRD PAIR AT THE SAME SCOPE, WHICH SCORES WHAT AN ANSWER TO THE SECOND WOULD COST. Where the
   surrogate merely TIES with the comparator, the design that answers it takes a CANDIDATE SET within a
   derived margin of the surrogate's extremum and re-compares the survivors through `flow_weight` — which
   solver/flow.c proves returns the full scan's member pointer for pointer, so it is exact and needs no edit
   to the order. `keyIndexBandMembersLifetime` over `keyIndexBandWeighedLifetime` is the SHARE OF THE
   FRONTIER that set holds, which is the number that decides whether an index narrows anything: a small
   share is an index worth building, and a share near one is one that saves nothing and whose per-ask cost is
   the walk the order already performs.
   READ THEM AS A FRACTION. The numerator alone is a count over a frontier whose size this stream publishes
   separately and which GROWS, so a lifetime numerator against a terminal gauge under-reads by the factor the
   frontier grew — the denominator is raised on the same walk over the same population precisely so nobody
   has to construct one. Both are LIFETIME counts and may be differenced; `keyIndexAskedLifetime` is the
   reachability witness for both. */
const KEYCHK = ["keyArmedLifetime", "keyStaleGenLifetime", "keyFirstSeenLifetime", "keyRunningLifetime",
                "keyIndexAskedLifetime", "keyIndexDifferedLifetime",
                "keyIndexDifferedTieLifetime", "keyIndexDifferedStrictLifetime",
                "keyIndexBandMembersLifetime", "keyIndexBandWeighedLifetime"];

/* WHAT THE ORDER IS MADE OF, WHICH EVERY ROW ABOVE PRESUPPOSES AND NONE OF THEM ASKS. The scopes above say
   what the order DECIDED, what it COST and whether its key stands still.  These six say whether it can
   separate anybody at all: `wTop - wMin` is the whole frontier's spread in the order's own points,
   `nonrewardMax` is the bound every term except the reward is under, and `visMin`/`visMax` and `distMax` are
   the spreads of two of those terms — the optimism denominator and the fitness reading.

   EVERY ONE OF THEM IS READ BY `engine/build.mjs` AND BY NOTHING ELSE, which drives the SMOKE FIXTURE.  So
   the question a tied frontier actually poses — what breaks the tie among fourteen thousand members standing
   at one weight — was measurable only on a document whose fork factor and member count are its author's
   design decisions.  That is the same defect as the minter triple and the cost scope, one scope over again.

   WHY THESE SIX AND WHY AS A GROUP.  `neverPickedGap` above reads 0.000 on a real page and 73-93% of members
   stand tied at the top, which is a statement that SOMETHING ELSE decided; it does not say what, and no row
   above can.  A term with `min == max` across the frontier is a COMMON OFFSET and orders nobody, so the pair
   is the reading and either half alone is not: `visMax - visMin` at zero says the optimism term separated
   nothing, and `wTop - wMin` at zero says the whole order did.  A fork COPIES its parent's coordinates
   verbatim (flow_fork_inherit), so a frontier grown by forking can carry one value for a term on every
   member, which is the state that makes registry order the arbiter.

   ALL SIX ARE GAUGES over the members standing NOW and may NOT be differenced — an extremum over a set that
   grows and whose members are forked from one another moves for reasons that are not a rate.  They are
   printed RAW with no spread composed here: result.c prescribes reading `wTop` against `wMin` and names
   `nonrewardMax` as what bounds the non-reward terms, and a driver that subtracted them would be publishing
   an arithmetic the contract states in points whose unit a reader has to hold separately. */
const SPREAD = ["wTop", "wMin", "nonrewardMax", "visMin", "visMax", "distMax"];

/* THE SAME TWO-WAY DERIVED CHECK, for the third time and for the reason it exists: a row named here that
   result.c renames throws, and a spread row result.c ADDS that this file does not name throws too.  The
   pattern is spelled from the six rather than from a prefix regex because this scope's names share no stem —
   which is itself why they went unread: there was no shape to notice them by. */
function spreadScope() {
  const keys = wfqComposerKeys();
  for (const k of SPREAD)
    if (!keys.has(k))
      throw new Error("[live-wfq] result_wfq_json no longer publishes `" + k + "` — this driver names it " +
                      "from result.c's own composer, so a row that has gone is one the producer renamed or " +
                      "dropped rather than one this file invented.");
  return SPREAD;
}

/* THE SAME TWO-WAY CHECK AGAIN, AGAINST THE SAME COMPOSER AND FOR THE SAME REASON. */
function keyScope() {
  const keys = wfqComposerKeys();
  for (const k of KEYCHK)
    if (!keys.has(k))
      throw new Error("[live-wfq] result_wfq_json no longer publishes `" + k + "` — this driver names it " +
                      "from result.c's own composer, so a row that has gone is one the producer renamed " +
                      "or dropped rather than one this file invented.");
  const published = [...keys].filter((k) => /^key[A-Z]/.test(k));
  const unread = published.filter((k) => !KEYCHK.includes(k));
  if (unread.length)
    throw new Error("[live-wfq] result_wfq_json publishes key-check row(s) no reader here names: " +
                    unread.join(", ") + ". result.c says the four PARTITION the classifications, so a fifth " +
                    "arm that nothing names is an exemption absorbing comparisons with no row to say so.");
  return KEYCHK;
}

/* AND THE SAME TWO-WAY CHECK THE BRANCH SCOPE GETS, for the same reason and against the same composer.
   A row named here that result.c has renamed throws naming the row; a cost-scope row result.c ADDS that
   this file does not name throws too — because a published row with no reader is exactly what this whole
   block exists to end, and a one-way check is how it recurs. */
function costScope() {
  const keys = wfqComposerKeys();
  for (const k of COST)
    if (!keys.has(k))
      throw new Error("[live-wfq] result_wfq_json no longer publishes `" + k + "` — this driver names it " +
                      "from result.c's own composer, so a row that has gone is one the producer renamed " +
                      "or dropped rather than one this file invented, and every reading below it would " +
                      "compare an absent field as undefined.");
  /* THE SHAPES THIS SCOPE OWNS, AND THE THIRD ONE IS WHY THE FILTER IS NOT A CONVENTION. The two-way check
     above can only see a published row it RECOGNISES as cost-scope, so a row added under a naming
     convention this pattern does not match is not caught by it — it is absent from the guard, absent from
     `COST`, computed on every census of every run and read by nobody, which is precisely the state the
     guard exists to end arriving through the guard's own selector. `rivalMiss` is named here for that
     reason: the partition is cost-scope by its subject and not by its spelling. */
  const published = [...keys].filter((k) => /^(?:scan[A-Z]|preemptAsks|rivalMiss)/.test(k));
  const unread = published.filter((k) => !COST.includes(k));
  if (unread.length)
    throw new Error("[live-wfq] result_wfq_json publishes cost-scope row(s) no reader here names: " +
                    unread.join(", ") + ". These rows had NO live-page reader at all until this list " +
                    "existed — name the row and say what it means, or the scope goes back to being " +
                    "measurable only on the fixture that chose its own frontier.");
  return COST;
}
/* THE EPOCH FAMILY'S OWN TWO-WAY CHECK, AND IT IS OWED FOR THE REASON THE COST SCOPE'S IS. The four scopes
   above each guard one family of published rows in both directions, and the `epoch*` family matched NONE of
   their selectors -- so these four rows were named by hand in COUNTERS and GAUGES with nothing comparing
   that list against the producer, and a FIFTH epoch row added to result.c would be composed on every census
   of every run and read by nobody.  That is precisely the state the other four guards exist to end,
   arriving through the gap between their selectors rather than through any of them.
   IT IS ONE FAMILY AND NOT A SWEEP OF THE UNGUARDED REMAINDER, deliberately: the rows this driver now
   composes a reading from are these, and a guard over every published row nobody has partitioned is the
   uniform repair CLAUDE.md forbids.  The remainder is REPORTED rather than swept.
   DERIVED FROM THE COMPOSER and never from a count: a renamed row throws naming the row, and a published
   `epoch*` row no reader here names throws too. */
function epochScope() {
  const keys = wfqComposerKeys();
  const named = [...COUNTERS, ...GAUGES].filter((k) => /^epoch[A-Z]/.test(k));
  for (const k of named)
    if (!keys.has(k))
      throw new Error("[live-wfq] result_wfq_json no longer publishes `" + k + "` — this driver takes the " +
                      "epoch family from result.c's own composer, so a row that has gone is one the producer " +
                      "renamed or dropped rather than one this file invented, and both readings composed " +
                      "from it would divide an absent field as undefined.");
  const published = [...keys].filter((k) => /^epoch[A-Z]/.test(k));
  const unread = published.filter((k) => !named.includes(k));
  if (unread.length)
    throw new Error("[live-wfq] result_wfq_json publishes epoch-scope row(s) no reader here names: " +
                    unread.join(", ") + ". Name the row in COUNTERS or GAUGES with its KIND, because a row " +
                    "whose kind a reader cannot take from this output is one they are not entitled to do " +
                    "arithmetic on.");
  return named;
}
/* ── THE HEAP CENSUS'S BYTE ROWS, WHICH HAVE NEVER BEEN READ ON A REAL PAGE ───────────────────────────────
   WHY THIS SCOPE EXISTS AND WHY IT IS THIS DRIVER'S. bridge.js composes `heap: result._heap` on the SAME
   `_engineLog` row it composes `wfq: result._wfq` on, and DCHECKs `_heap`'s presence on every result
   document it accepts — and it says in its own words why that matters: "Until they rode this document they
   were printed only by the smoke driver's loop, so every one of these numbers had been quoted about one
   fixture and never once about a real page". The producer is built and the row arrives; `sample()` below
   returned `wfq` and `quantum` and dropped `heap` on the floor, so the published census had no live-page
   reader at all. That is the write-with-no-reader half of the record-field contract, one census over from
   the minter triple and the cost scope, and it is caught the same way those are.

   WHAT THE ABSENT READING COSTS, WHICH IS WHY THESE ELEVEN AND NOT THE OTHER TWENTY-ONE. `engine/build.mjs`
   links the shipped artifact with `-sMAXIMUM_MEMORY=4294967296` and calls that line "THE ARCHITECTURE'S
   CEILING, not a budget", because wasm32 addresses 4 GiB and nothing can raise it. Whether a real page's
   frontier approaches that ceiling is a question about BYTES, and the only bytes figures the engine
   publishes are on this census. The fixture's are measured and quoted in three places in build.mjs; a real
   page's have never been taken, so the one question the ceiling poses has been answered for the one
   document whose workload is its author's design decision and for nothing else.

   IT IS A DIFFERENT OBJECT FROM `wfq` AND IS THEREFORE PRINTED BEFORE THE EMPTY-FRONTIER RETURN. result.c
   composes `{"members":0}` with no branch row on a drained or parked frontier, and the loop below returns
   early on that; the heap census is composed by `result_heap_json` regardless and is at its most
   interesting on exactly that path, since `arenaKiB` is a high-water that a drain does not give back. A
   block placed after that return would lose the reading where it is worth most. */
function heapComposerKeys() {
  const src = fs.readFileSync(RESULT_C, "utf8");
  const i = src.indexOf("char *result_heap_json(JSContext *ctx)");
  if (i < 0) throw new Error("[live-wfq] cannot find `result_heap_json` in " + RESULT_C + " — this driver " +
                             "takes its byte-row set from that composer, and a census it cannot read is " +
                             "one whose rows it would compare as undefined.");
  const j = src.indexOf("\n}\n", i);
  if (j < 0) throw new Error("[live-wfq] `result_heap_json` in " + RESULT_C + " has no closing brace at " +
                             "column 0 — the span this reader derives its row set from is unbounded.");
  return new Set([...src.slice(i, j).matchAll(/\\"([A-Za-z_][A-Za-z0-9_]*)\\":/g)].map((m) => m[1]));
}

/* THE KINDS, FROM result.c's AND engine.c's OWN PROSE, because a byte row's kind decides whether the
   ceiling question may be asked of it at all and no format string carries that.
   EVERY ROW HERE IS A GAUGE AT THE CENSUS INSTANT EXCEPT ONE. The quickjs rows are `JS_ComputeMemoryUsage`
   fields read at that instant; `unattributed` is, in result.c's words, "a SUBTRACTION of two gauges"; and
   `cLiveKiB` is `mallinfo().uordblks`, what the allocator "has handed out and not been given back", which
   FALLS when memory is freed. None of them may be differenced across two rows of this stream as a rate,
   and a maximum over this stream's samples of any of them UNDER-READS the true peak, because a peak
   between two censuses is invisible to a sampler.
   `arenaKiB` IS THE EXCEPTION AND IT IS THE ONLY ROW THE CEILING QUESTION CAN BE ASKED OF. engine.c, at
   `engine_c_alloc_arena`: it is "the total space it has taken from the system, which in wasm is LINEAR
   MEMORY AND ONLY EVER GROWS". Monotone means its latest value IS its high-water, which is what a ceiling
   is compared against — and it is monotone on exactly the host whose ceiling is in question, since this
   driver attaches to the extension in a real browser and therefore always samples the wasm artifact. On a
   NATIVE run the same row can fall, so this kind statement is about this driver's path and not about the
   field everywhere. */
const HEAP_GAUGE = ["miscBytes", "objBytes", "propBytes", "shapeBytes", "strBytes", "atomBytes",
                    "funcBytes", "arrayElemBytes", "unattributed", "cLiveKiB"];
const HEAP_WASM_MONOTONE = ["arenaKiB"];

function heapScope() {
  const keys = heapComposerKeys();
  const named = [...HEAP_GAUGE, ...HEAP_WASM_MONOTONE];
  for (const k of named)
    if (!keys.has(k))
      throw new Error("[live-wfq] result_heap_json no longer publishes `" + k + "` — this driver names it " +
                      "from result.c's own composer, so a row that has gone is one the producer renamed " +
                      "or dropped rather than one this file invented, and every reading below it would " +
                      "compare an absent field as undefined.");
  /* THE TWO-WAY CHECK, FILTERED TO THE BYTE FAMILY BY SUBJECT AND NOT BY SPELLING — the distinction
     `costScope` already makes. A byte row added to that composer and named by nobody here is the exact
     state this block exists to end, and the ceiling question is the one a new byte row is most likely to
     be about. The twenty-one rows this filter excludes are counts and histograms (allocations, atoms,
     realm refs, step machines, tramp frames): real rows, a different question, and not this scope's. */
  const published = [...keys].filter((k) => /(?:Bytes|KiB)$/.test(k) || k === "unattributed");
  const unread = published.filter((k) => !named.includes(k));
  if (unread.length)
    throw new Error("[live-wfq] result_heap_json publishes byte row(s) no reader here names: " +
                    unread.join(", ") + ". A published byte row with no reader is how the heap census came " +
                    "to be composed on every real-page document and read on none — name the row and say " +
                    "which kind it is, or the ceiling question grows a column nobody asks.");
  return named;
}

/* THE CEILING, DERIVED FROM THE LINK FLAG THAT SETS IT RATHER THAN COPIED AS A NUMBER. A restated constant
   is a second copy, and the copy that drifts is the one nobody runs against reality: a build that raises or
   lowers `-sMAXIMUM_MEMORY` must move this fraction's denominator with it, and a hardcoded 4 GiB would go
   on reporting a share of a ceiling the artifact no longer has. Exactly one occurrence is required — two
   would mean the link line this reader is about is no longer the only one, and a share composed against
   whichever matched first would be a fraction of a ceiling some other target was linked with. */
const BUILD_MJS = path.join(__dirname, "..", "engine", "build.mjs");
function wasmCeilingKiB() {
  const hits = [...fs.readFileSync(BUILD_MJS, "utf8").matchAll(/"-sMAXIMUM_MEMORY=(\d+)"/g)];
  if (hits.length !== 1)
    throw new Error("[live-wfq] engine/build.mjs carries " + hits.length + " `-sMAXIMUM_MEMORY=` flag(s) " +
                    "and this reader composes a share of the ONE ceiling the sampled artifact was linked " +
                    "with. Zero means the flag was renamed and the share would have no denominator; more " +
                    "than one means it is no longer one ceiling and a reader cannot tell which was used.");
  return Number(hits[0][1]) / 1024;
}

/* AND THE NUMERATOR THAT ANSWERS THAT CEILING, WHICH IS NOT THE ENGINE'S TO EMIT AND SO IS NOT DERIVED FROM
   result.c AT ALL. `workingSetBytes` is `M.HEAPU8.length` — the view over the instance's ENTIRE linear memory,
   stated by renderer.html on every ABI reply, declared by mojom.js and carried onto the run row by bridge.js's
   `linesToAnalysis`. It is checked by NAME here rather than derived, because there is no composer block to
   parse: this is a PRESENCE check over one spelling and therefore a FLOOR, not a derivation — it catches the
   field being renamed or dropped and nothing else.
   IT IS WORTH MAKING EXACTLY BECAUSE bridge.js IS INTERPRETED FROM THE TREE. The wasm this driver samples is
   live only after a build, so a source check against the engine can disagree with the artifact in the browser;
   the trusted zone is DEPLOYED ON WRITE, so the file this reads and the code that produced the row are the same
   bytes. Without it a renamed field reads as `null` on every row, which this stream spells as "the producer
   never sent it" — the absence of a producer and a producer that stopped stating it rendering alike, which is
   the one distinction the rest of this file spends its nulls to keep. It throws at startup rather than after a
   window of samples, for the reason every other scope here is derived before the browser is touched. */
const BRIDGE_JS = path.join(__dirname, "..", "extension", "bridge.js");
function assertWorkingSetRow() {
  const hits = [...fs.readFileSync(BRIDGE_JS, "utf8").matchAll(/\bworkingSetBytes:/g)];
  if (hits.length !== 1)
    throw new Error("[live-wfq] extension/bridge.js composes " + hits.length + " `workingSetBytes:` row(s) " +
                    "and this reader samples the ONE the run record carries. Zero means the field was renamed " +
                    "or dropped, and every ceiling share below it would read `null` as though the trusted zone " +
                    "had never stated the instance's working set; more than one means the row is no longer the " +
                    "only place that name is composed and a reader cannot tell which one reached the log.");
}

/* THE ONE COST IDENTITY result.c STATES AS CHECKABLE ON THIS DOCUMENT, in the same three-state shape the
   branch identities use and for the same reason: it is a DCHECK in flow_wfq_census, which is compiled OUT
   of the release build this driver samples, and result.c says in as many words that a break makes the
   quotient "a fraction of some other population". It is COPIED from the engine rather than composed here
   — an auditor derives its rule from the code that owns it, and a restated invariant is a second copy. */
const COST_IDENTITIES = [
  ["scanRivalRuns<=preemptAsksLifetime", ["scanRivalRuns", "preemptAsksLifetime"],
   (w) => w.scanRivalRuns <= w.preemptAsksLifetime],
  /* AND THE PARTITION'S OWN, WHICH IS AN EQUALITY WHERE THE ONE ABOVE IS A CONTAINMENT — result.c asserts it
     at the census and that DCHECK is compiled OUT of the release build this driver samples, so it is checked
     here for the same reason the branch identities are. It is the one thing that makes the three rows a
     partition rather than three opinions: a sum BELOW `scanRivalRuns` is an arm moved off the line that
     walks, a sum ABOVE it is a miss counted where no walk followed, and either way the reading the rows
     exist for — which invalidator a rescan would have to lose to not happen — is no longer answerable. */
  ["rivalMissGen+rivalMissCur+rivalMissBoth==scanRivalRuns",
   ["rivalMissGen", "rivalMissCur", "rivalMissBoth", "scanRivalRuns"],
   (w) => w.rivalMissGen + w.rivalMissCur + w.rivalMissBoth === w.scanRivalRuns],
  /* AND THE AWAY POPULATION'S, WHICH IS WHAT MAKES `epochRebuildLifetime` A MEASUREMENT RATHER THAN A
     COUNTER WITH NOTHING UNDER IT.  The left side is maintained by four incremental statements in
     solver/flow.c and the right side is walked by the census, so this is two maintainers compared at one
     instant.  A break means a member crossed its family's epoch base at a site the incremental count does
     not name, and the lifetime rebuild — which is this same quantity sampled at every emission — is then
     wrong by however much this is.  It is filed with the COST identities and not the branch ones because
     the quantity it guards is read against `scanNextWeights`, which is on this scope. */
  ["epochAwayLive==epochAwayWalk", ["epochAwayLive", "epochAwayWalk"],
   (w) => w.epochAwayLive === w.epochAwayWalk],
];

/* THE IDENTITIES result.c STATES AS CHECKABLE ON THIS DOCUMENT, checked rather than trusted — the same
   treatment `switchDelta` already gets below and for the same reason: a violated one means every other
   branch row on the line is a number about nothing. They are the engine's own DCHECKs in flow_wfq_census,
   which are compiled out of the release build this driver samples. */
/* EACH CARRIES ITS OPERANDS, AND AN IDENTITY WHOSE OPERANDS ARE NOT ALL NUMBERS IS REPORTED AS UNJUDGED
   RATHER THAN AS HELD. Two absent fields compare EQUAL in JavaScript, so a predicate written as a bare
   `===` passes on a census carrying neither — an assert whose two sides cannot disagree, which is not a
   weak check but a non-check that prints like a passing one. The three states are kept apart on the row
   (`brIdent` null, a list of broken names, or a list under `brUnjudged`) for the same reason every other
   absence in this stream is spelled null and never 0. */
const BR_IDENTITIES = [
  ["brLiveSum==members", ["brLiveSum", "members"],
   (w) => w.brLiveSum === w.members],
  ["brUsLifeSum+brRetiredUsLife==chargedUsLife", ["brUsLifeSum", "brRetiredUsLife", "chargedUsLife"],
   (w) => w.brUsLifeSum + w.brRetiredUsLife === w.chargedUsLife],
  ["brCrowdLive==brLiveMax", ["brCrowdLive", "brLiveMax"],
   (w) => w.brCrowdLive === w.brLiveMax],
  ["brHeldUsLife+brEmptyUsLife==brUsLifeSum", ["brHeldUsLife", "brEmptyUsLife", "brUsLifeSum"],
   (w) => w.brHeldUsLife + w.brEmptyUsLife === w.brUsLifeSum],
  ["brMinterLive+brMinterGoneLife==brBornLifeMax", ["brMinterLive", "brMinterGoneLife", "brBornLifeMax"],
   (w) => w.brMinterLive + w.brMinterGoneLife === w.brBornLifeMax],
  /* THE ONLY BOUND ON THE NUMERATOR OF THE READING THIS DRIVER EXISTS TO TAKE, AND IT WAS THE ONE
     IDENTITY NOT CHECKED HERE. The banner above gives the reason every other row on this list is
     checked -- the engine states these as DCHECKs in flow_wfq_census, which are compiled OUT of the
     release build this driver samples -- and that argument applies hardest to this one. The
     denominator side is already pinned twice over: brLiveSum==members fixes `members`, and the
     brMinterLive/brMinterGoneLife identity above fixes the pair against brBornLifeMax. The numerator
     side was a bare per-bucket load with nothing standing under it, so `minterUsShare` could be
     published as a fraction of a denominator its numerator is not drawn from -- which is the
     consequence flow.c names in its own words at the assert this line copies.
     IT IS COPIED FROM THE ENGINE AND NOT COMPOSED HERE, for the reason CLAUDE.md gives about an
     auditor deriving its rule from the code that owns it: a restated invariant is a second copy, and
     the copy that drifts is the one nobody runs against reality. Both conjuncts are one-sided because
     a maximum and a sum are upper bounds over sets the minter belongs to. */
  ["brMinterLive<=brLiveMax&&brMinterUsLife<=brHeldUsLife",
   ["brMinterLive", "brLiveMax", "brMinterUsLife", "brHeldUsLife"],
   (w) => w.brMinterLive <= w.brLiveMax && w.brMinterUsLife <= w.brHeldUsLife],
];

/* A SHARE IS PRINTED WHERE result.c PRESCRIBES THE QUOTIENT AND NOWHERE ELSE, which is the distinction the
   header makes about `starvedPicks / picksLifetime`: that one is withheld because it sums two populations,
   and these two are the readings result.c names in its own text — "Take the crowd's share of the members
   standing against its share of the thread the live buckets hold", and "READ `brMinterUsLife /
   brHeldUsLife` AGAINST `brMinterLive / members`". The three-way verdict those readings carry (at par /
   near zero / above par) is NOT computed: classifying would pick a threshold the contract leaves to a
   reader, and the three take opposite diffs.
   A QUOTIENT OF TWO BURNS IS UNIT-FREE AND A RAW BURN IS NOT. result.c: "A quotient of two burns from ONE
   run in ONE unit is the same number either way; a raw microsecond total from this line is not, and is
   quoted with that line beside it." So `isCpu` is carried on every row that prints one.
   A ZERO DENOMINATOR YIELDS null AND NEVER 0 — an unasked question and a measured zero are different
   facts, and this stream already spells absence as null everywhere else. */
const share = (n, d) => (typeof n === "number" && typeof d === "number" && d > 0
                           ? Number((n / d).toFixed(6)) : null);

async function connect() {
  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  return { extId: lock.extId,
           browser: await puppeteer.connect({ browserURL: `http://127.0.0.1:${lock.port}`,
                                              defaultViewport: null,
                                              targetFilter: (t) => t.type() !== "browser" }) };
}

async function offscreenPage(browser, extId) {
  const url = `chrome-extension://${extId}/ast-worker.html`;
  for (let i = 0; i < 60; i++) {
    const t = browser.targets().find((t) => t.url().startsWith(url));
    if (t) { const pg = await t.page().catch(() => null); if (pg) return pg; }
    await sleep(200);
  }
  throw new Error("no offscreen document — is the extension loaded?");
}

/* READ OFF THE RUN RECORD, which is the shipped path: bridge.js asserts `_wfq`'s presence and shape
   on every result document it accepts, and the popup's GET_ENGINE_RUNS reads the same array.
   A row with NO `wfq` at all and a row carrying `{members:0}` are DIFFERENT FACTS and are returned
   as different things — bridge.js's own contract for this field is that a missing census is a broken
   contract while `{members:0}` is an empty frontier, and collapsing them would report a scheduler
   that published nothing as one whose frontier had drained. */
function sample(pg) {
  return pg.evaluate(() => {
    const rows = (self._engineLog || []);
    if (!rows.length) return { NO_ROW: true };
    const r = rows[rows.length - 1];
    let sched = null;
    try { const p = self.rendererPoolProbe(); sched = { alive: p.scheduler.alive }; }
    catch (e) { sched = { PROBE_THREW: String((e && e.message) || e) }; }
    if (!("wfq" in r)) return { run: r.run, NO_WFQ: true, switches: r.switches, sched: sched };
    /* `quantum` rides the same log row (bridge.js composes it beside `wfq`) and its `isCpu` is what makes a
       RAW burn on this line quotable. Absent is returned as absent; bridge.js asserts its shape upstream. */
    /* AND THE HEAP CENSUS, WHICH RIDES THIS SAME ROW AND WAS DROPPED HERE. bridge.js composes
       `heap: result._heap` one line from the `wfq` it composes above and DCHECKs its presence on every
       document it accepts, so an absent one is a producer that stopped composing rather than an engine
       holding no bytes — returned as absent and never as an object of zeros, the rule every other
       absence in this driver follows. The NO_WFQ return above does NOT carry it: that path is bridge's
       `_wfq` assert already having failed, which is a broken relay rather than a reading of a page. */
    /* AND THE TRUSTED ZONE'S OWN READING OF THE INSTANCE, WHICH RIDES THIS SAME ROW AND IS NOT PART OF THE
       HEAP CENSUS. bridge.js composes `workingSetBytes` beside `egressAsked` and asserts its presence on the
       arm that builds this row, so an absent one is that producer having stopped composing it rather than an
       instance holding no bytes. Returned as absent and never as a zero: zero bytes of linear memory is not a
       state a running module can be in, so a 0 here could only ever be a defaulted hole. */
    return { run: r.run, wfq: r.wfq, switches: r.switches, sched: sched,
             quantum: ("quantum" in r) ? r.quantum : null,
             heap: ("heap" in r) ? r.heap : null,
             workingSetBytes: ("workingSetBytes" in r) ? r.workingSetBytes : null };
  });
}

async function main() {
  const urls = process.argv.slice(2);
  if (!urls.length) { console.error("usage: node testing/live-wfq.js <url> [url…]"); process.exit(2); }
  /* DERIVED BEFORE THE BROWSER IS TOUCHED, so a renamed or reader-less row fails here rather than after a
     window of samples has been spent on a census this driver cannot describe. */
  const BR = branchScope();
  const COSTROWS = costScope();
  const KEYROWS = keyScope();
  const SPREADROWS = spreadScope();
  const EPOCHROWS = epochScope();
  const HEAPROWS = heapScope();
  const CEIL_KIB = wasmCeilingKiB();
  assertWorkingSetRow();
  console.log("# artifact " + JSON.stringify(artifactStamp()));
  console.log("# windowMs=" + WINDOW + " everyMs=" + EVERY +
              " — COUNTERS (may be differenced): " + COUNTERS.join(",") +
              " | GAUGES (may FALL; never difference): " + GAUGES.join(","));
  console.log("# branch scope, derived from result_wfq_json (" + BR.length + " rows) — DIFFERENCEABLE: " +
              BR_DIFFABLE.join(",") + " | AT THIS INSTANT ONLY (the bucket SELECTED moves between " +
              "samples; never difference): " + BR_INSTANT.join(","));
  console.log("# crowd is selected by brLiveMax (membership); minter by brBornLifeMax (mint). They are ONE " +
              "bucket only while nothing has departed — `selectorsAgree` says whether they are, on each row.");
  console.log("# cost scope, derived from result_wfq_json (" + COSTROWS.length + " rows) — ALL LIFETIME " +
              "COUNTS, all differenceable: " + COSTROWS.join(",") + " | what asking the order COST, which " +
              "every row above is silent about because every row above is what it DECIDED. Read " +
              "`scanNextWeights`/`scanNextRuns` against `members`: a dispatch scan that weighs a constant " +
              "number of members and one that weighs the frontier are different costs and only the second " +
              "grows with it. `scanNextWeights / steps` and `scanRivalRuns / forks` need @COLD's `steps` " +
              "and `forks` and are NOT composed here.");
  console.log("# key-check scope, derived from result_wfq_json (" + KEYROWS.length + " rows) — ALL LIFETIME " +
              "COUNTS: " + KEYROWS.join(",") + " | the SCORE of flow_pick's member-key invariant, which is a " +
              "PREDICTED ABSENCE: a run in which it never fires is satisfied identically by an invariant that " +
              "holds and by a walk that compared nothing. `keyArmedLifetime` is the comparisons actually " +
              "made; the other three are the exemptions that absorbed the rest. Raised under APICLIENT_DEV, " +
              "so four zeros are a question about the BUILD before they are a question about the run.");
  console.log("# order-spread scope, derived from result_wfq_json (" + SPREADROWS.length + " rows) — ALL " +
              "GAUGES, never differenced: " + SPREADROWS.join(",") + " | what the order is MADE OF, which " +
              "every row above presupposes and none asks. A term whose min equals its max across the " +
              "frontier is a COMMON OFFSET and orders nobody; `wTop`-`wMin` is the whole spread in the " +
              "order's own points and `nonrewardMax` is the bound every term but the reward is under. Read " +
              "beside `neverPickedGap`: a gap of 0.000 says something else decided, and these say whether " +
              "anything could have.");
  console.log("# epoch scope, derived from result_wfq_json (" + EPOCHROWS.length + " rows) — the KIND is in " +
              "each key: " + EPOCHROWS.join(",") + ". `epochRebuildLifetime` is the whole maintenance an " +
              "index over solver/flow.c's `flow_index_key` would pay, summed AT EACH EMISSION; the two " +
              "`Away` gauges are its shape at one instant and are a CONSERVATION IDENTITY, never the cost. " +
              "Both readings are composed per census below: `epochRebuildWalkShare` against " +
              "`scanNextWeights` — the walk an index would REPLACE — where well below it the epoch is a " +
              "COST and an index narrows, and at or above it the rebuild is the walk moved rather than " +
              "removed; and `epochRebuildPerEmit`, which tells a large total over many cheap emissions " +
              "from a small one over few expensive ones. A ZERO IS READ AGAINST `epochResetsLifetime` " +
              "FIRST: zero resets is a run that never emitted, and the pair is then SILENT about the design " +
              "rather than favourable to it.");
  console.log("# heap scope, derived from result_heap_json (" + HEAPROWS.length + " byte rows) — GAUGES at " +
              "the census instant, never differenced: " + HEAP_GAUGE.join(",") + " | MONOTONE IN WASM, so " +
              "its latest value IS its high-water: " + HEAP_WASM_MONOTONE.join(",") + " | ceiling " +
              CEIL_KIB + " KiB, derived from engine/build.mjs's own -sMAXIMUM_MEMORY. `arenaCeilingShare` " +
              "is a FLOOR and not the artifact's share of its address space: it is the C ALLOCATOR's arena " +
              "over that ceiling, and the linear memory also holds the stack and static data.");
  /* AND THE SHARE THAT IS NOT A FLOOR, WHICH IS A DIFFERENT OBSERVER AND THEREFORE A DIFFERENT BANNER. The
     sentence above used to end "a reader wanting the true share is waiting on that field reaching this row",
     and this is that field arriving: bridge.js composes `workingSetBytes` onto the run row beside its egress
     census, so the NUMERATOR of the real question is now on the line this driver samples. It is printed under
     its own banner rather than folded into the heap one because the heap rows are DERIVED from result_heap_json
     and this is not a row the engine emits — naming it there would put a row with no producer inside the one
     scope whose producer is checked. */
  console.log("# working set, carried by bridge.js off every ABI reply (renderer.html's HEAPU8.length) — ONE " +
              "row, MONOTONE: a wasm Memory never shrinks, so its latest value IS its high-water and " +
              "`workingSetCeilingShare` over the " + CEIL_KIB + " KiB ceiling is a high-water question. It is " +
              "the TRUE share where `arenaCeilingShare` is a floor — the arena is a SUBSET of this, which also " +
              "holds the stack and static data. ABSENT IS null AND NEVER 0: a running module cannot hold zero " +
              "bytes of linear memory, so a 0 could only be a defaulted hole. NOT DIVIDED BY ANY heap ROW: this " +
              "is read at the END of the last round and that census is the instant qjs_emit_partial composed " +
              "it, so a quotient of the two is a two-moments figure — the columns go side by side and the " +
              "reader divides knowing what is in it. The denominator is the INSTANCE's and not the pool's: " +
              "bridge.js sums this same number across live engines against a DEVICE RAM floor, and the wasm32 " +
              "ceiling is per MODULE, so that sum is a fraction of the wrong denominator.");

  const { browser, extId } = await connect();
  try {
    const pg = await offscreenPage(browser, extId);
    const pages = await browser.pages();
    const nonExt = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                       !p.url().startsWith("devtools://"));
    const page = nonExt.length ? nonExt[nonExt.length - 1] : await browser.newPage();

    for (const url of urls) {
      try { await page.goto("about:blank", { waitUntil: "domcontentloaded", timeout: 15000 }); } catch (e) {}
      const t0 = Date.now();
      let nav;
      try { const r = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
            nav = r ? r.status() : "nav-noop"; }
      catch (e) { nav = "navfail:" + String((e && e.message) || e).split("\n")[0]; }
      console.log("\n═══ " + url + "  nav=" + nav);

      let n = 0;
      /* THE CENSUS AT WHICH THIS RUN'S TWO BRANCH SELECTORS FIRST SEPARATED, WHICH IS THE ONE THING A READER
         OF THE TERMINAL LINE COULD NOT ASK. `selectorsAgree` below is a per-census GAUGE over a row that may
         FALL, and this stream is read by taking its LAST line -- so a run that separated and then re-agreed
         reads, at terminal, exactly like a run that never separated at all. Those are opposite facts: the
         first is the only sample the minter triple can say anything from, and the second is the whole archive.
         MEASURED over 809 archived censuses: ONE separated, and it is census #2 OF 3 with #3 agreeing, so a
         terminal reading misses the single separable sample there has ever been. This is a LIFETIME landmark
         beside that gauge and it is monotone: once set it never clears.
         ITS WINDOW IS NAMED AND IS THIS RUN'S CENSUSES UP TO AND INCLUDING THIS LINE, because a landmark whose
         window goes unstated reads as a claim about all time -- so `selectorsCensusesSeen` rides beside it and
         a null with a count of 1 is a run that has barely started, never a run that agreed throughout. */
      let sepAt = null;
      while (Date.now() - t0 < WINDOW) {
        await sleep(EVERY);
        const s = await sample(pg);
        n++;
        if (s.NO_ROW) { console.log(JSON.stringify({ n, atMs: Date.now() - t0, NO_ROW: true })); continue; }
        if (s.NO_WFQ) { console.log(JSON.stringify({ n, atMs: Date.now() - t0, run: s.run, NO_WFQ: true })); continue; }
        const w = s.wfq;
        const out = { n, atMs: Date.now() - t0, run: s.run,
                      alive: s.sched && s.sched.alive };
        for (const k of COUNTERS) out[k] = (k in w) ? w[k] : null;   // null = absent, never 0
        for (const k of GAUGES) out[k] = (k in w) ? w[k] : null;
        /* THE IDENTITY result.c STATES, CHECKED RATHER THAN TRUSTED. Non-zero means the census and
           the run record it rides on disagree about how many dispatches happened, and then every
           other row on this line is a number about nothing. */
        out.switchDelta = (typeof w.picksLifetime === "number" && typeof s.switches === "number")
                            ? w.picksLifetime - s.switches : null;
        /* result.c STATES THIS AS AN IDENTITY OF ONE EVALUATION — both counters are raised at the same line
           under the same condition — so a violation means they have stopped counting the same event and the
           subset is no longer a subset of anything.  Three-state like every other identity here: a list when
           it breaks, null when it holds, and `unjudged` when an operand is not a number, because two absent
           fields compare equal and a bare `<=` would pass on a census carrying neither. */
        out.starvedIdent = (typeof w.starvedPicks === "number" && typeof w.starvedPicksIdle === "number")
                             ? (w.starvedPicksIdle <= w.starvedPicks ? null : ["starvedPicksIdle<=starvedPicks"])
                             : undefined;
        if (out.starvedIdent === undefined) { delete out.starvedIdent; out.starvedUnjudged = true; }
        /* THE COST SCOPE, EMITTED BEFORE THE BRANCH BLOCK because that block returns early on an empty
           frontier and these rows are absent on exactly the same path — result.c composes `{"members":0}`
           with no term rows of any kind — so a reader meeting `costAbsent` is being told the short form
           was published and not that a walk read zero. */
        if (!("scanNextRuns" in w)) { out.costAbsent = true; }
        else {
          for (const k of COST) out[k] = (k in w) ? w[k] : null;
          const cbroke = [], cunjudged = [];
          for (const [name, operands, ok] of COST_IDENTITIES) {
            if (operands.some((k) => typeof w[k] !== "number")) { cunjudged.push(name); continue; }
            if (!ok(w)) cbroke.push(name);
          }
          out.costIdent = cbroke.length ? cbroke : null;
          out.costUnjudged = cunjudged.length ? cunjudged : null;
          /* THE THREE QUOTIENTS result.c PRESCRIBES AND WHOSE OPERANDS ARE BOTH ON THIS LINE, AND NO
             OTHERS — the same rule the crowd/minter shares are printed under. result.c also names
             `scanNextWeights / steps` against `members` and `scanRivalRuns` against `forks`; `steps` and
             `forks` are on the @COLD line and the run record, not here, so those two are NOT composed
             from this document and a reader wanting them joins the two censuses that share a `workDone`.
             `scanNextRuns`/`scanNextWeights` are therefore printed RAW beside `members`, which is this
             driver's standing rule: the columns go side by side and the reader does the division knowing
             what is in it. A ZERO DENOMINATOR YIELDS null AND NEVER 0. */
          out.censusMeanFrontier = share(w.scanCensusWeights, w.scanCensusRuns);
          out.censusWeighShare   = share(w.scanCensusWeights, w.scanNextWeights);
          out.rivalMissRate      = share(w.scanRivalRuns, w.preemptAsksLifetime);
          /* THE TWO READINGS result.c PRESCRIBES FOR THE EPOCH, COMPOSED HERE AND NOT WITH THE COUNTERS
             ABOVE, because one of the denominators is a COST row: result.c emits the epoch rows and both
             `scanNext*` rows in ONE `composef` past its empty-frontier return, so they are present and
             absent TOGETHER and this gate is exactly their gate. Composed after that return in the heap
             block's sense would be composed where neither operand exists.
             BOTH ARE LIFETIME OVER LIFETIME, which is what makes them readings rather than lotteries. The
             numerator is summed at each EMISSION and the denominators are lifetime counts of the same run,
             so no operand is an instant and neither quotient is the gauge-denominated under-read a lifetime
             total over a terminal `members` would be. The `Away` gauges are deliberately NOT divided into
             anything here: a census lands at an arbitrary point between two emissions, so that population
             reads near zero just after one and at its peak just before.
             THE THREE-WAY VERDICT IS NOT COMPUTED, for the reason the crowd and minter shares state: well
             below / at / above `scanNextWeights` take different diffs, and classifying would pick a
             threshold result.c's contract leaves to a reader. A ZERO DENOMINATOR YIELDS null AND NEVER 0. */
          out.epochRebuildWalkShare = share(w.epochRebuildLifetime, w.scanNextWeights);
          out.epochRebuildPerEmit   = share(w.epochRebuildLifetime, w.epochResetsLifetime);
        }
        /* THE SCORE OF THE WALK'S OWN PREDICTED ABSENCE, printed RAW and with no quotient composed from it.
           result.c prescribes reading `keyArmedLifetime` as the score and the other three as why, and states
           no fraction over them; the four are emitted together and are read together or not at all. */
        if (!("keyArmedLifetime" in w)) { out.keyAbsent = true; }
        else for (const k of KEYCHK) out[k] = (k in w) ? w[k] : null;
        /* WHAT THE ORDER IS MADE OF — printed beside `neverPickedGap` and the tie rows above, because a
           frontier standing at one weight is a statement that something else decided and these are what say
           whether any term could have.  Absent is null, never 0. */
        for (const k of SPREAD) out[k] = (k in w) ? w[k] : null;
        /* AN EMPTY FRONTIER PUBLISHES `{"members":0}` AND NO BRANCH ROW AT ALL, which is not the same fact
           as a branch scope reading zero — result.c composes that short form on its own path. Absent is
           said once, as a flag; filling twenty-three nulls would render an unasked question exactly like a
           measured one, which is the defect the rest of this stream spells with null to avoid. */
        /* THE HEAP CENSUS, PRINTED BEFORE THE EMPTY-FRONTIER RETURN BELOW because it is a different
           object composed by a different function and a drained frontier does not make it absent — see
           heapScope's banner. Absent is said ONCE as a flag rather than as eleven nulls, which is the
           rule the branch block below states: filling a row per field would render a census the producer
           never sent exactly like one it sent reading zero. */
        /* THE TRUSTED ZONE'S OWN BYTE FIGURE, PRINTED OUTSIDE THE HEAP GATE BECAUSE IT IS NOT IN THAT CENSUS
           AND DOES NOT ARRIVE OR GO MISSING WITH IT. One row, so absence is said ONCE as `null` and there is
           no flag to add: the rule the blocks around this one state — absent is said once rather than as a
           field per row — is satisfied by the null itself when the population is one. THE SHARE IS COMPOSED
           HERE AND IT IS THE ONLY QUOTIENT THIS ROW HAS: the numerator is MONOTONE (a wasm Memory never
           shrinks) and the denominator is a LINK CONSTANT, so neither operand is an instant and the ceiling
           question is sound — which is exactly what `arenaCeilingShare` can only bound from below. KiB on
           both sides, and the division is exact: bridge.js asserts the byte figure is a whole number of 64 KiB
           WASM pages where it reads it off the reply. A zero or absent denominator yields null and never 0. */
        out.workingSetBytes = (typeof s.workingSetBytes === "number") ? s.workingSetBytes : null;
        out.workingSetCeilingShare = (typeof s.workingSetBytes === "number")
                                       ? share(s.workingSetBytes / 1024, CEIL_KIB) : null;
        if (!s.heap || typeof s.heap !== "object") { out.heapAbsent = true; }
        else {
          const h = s.heap;
          for (const k of HEAPROWS) out[k] = (k in h) ? h[k] : null;
          /* THE ONLY QUOTIENT THIS SCOPE COMPOSES, AND IT IS ASKED OF THE ONE MONOTONE ROW. A share of a
             ceiling is a high-water question, so asking it of a GAUGE would publish whatever the allocator
             happened to be holding at this instant as a distance to a limit the run may already have been
             nearer to. `cLiveKiB` is printed RAW beside it for exactly that reason — this driver's standing
             rule that the columns go side by side and the reader divides knowing what is in it. A zero or
             absent denominator yields null and never 0. */
          out.arenaCeilingShare = share(h.arenaKiB, CEIL_KIB);
        }
        /* THE LANDMARK RIDES THE SHORT FORM TOO, because it is a fact about the RUN and not about this
           census's scope -- and the terminal census is free to be one that published no branch rows, which
           would otherwise take the one row a reader needs off the one line a reader takes. */
        if (!("branches" in w)) { out.brAbsent = true; out.selectorsEverSeparatedAt = sepAt;
                                  out.selectorsCensusesSeen = n;
                                  console.log(JSON.stringify(out)); continue; }
        for (const k of BR) out[k] = (k in w) ? w[k] : null;
        /* THE IDENTITIES, CHECKED. A violated one means every branch row on this line describes some other
           arrangement than the frontier it claims to, exactly as a non-zero switchDelta does above. */
        const broke = [], unjudged = [];
        for (const [name, operands, ok] of BR_IDENTITIES) {
          if (operands.some((k) => typeof w[k] !== "number")) { unjudged.push(name); continue; }
          if (!ok(w)) broke.push(name);
        }
        out.brIdent = broke.length ? broke : null;
        out.brUnjudged = unjudged.length ? unjudged : null;
        /* result.c's OWN two readings, and nothing else divided here. The crowd pair is what a fixture-scale
           refutation of the aging question is usually quoted from; the minter pair is the arm that question
           is actually about, and they are the same bucket only when `selectorsAgree`. */
        out.crowdLiveShare  = share(w.brCrowdLive,   w.members);
        out.crowdUsShare    = share(w.brCrowdUsLife, w.brHeldUsLife);
        out.minterLiveShare = share(w.brMinterLive,  w.members);
        out.minterUsShare   = share(w.brMinterUsLife, w.brHeldUsLife);
        /* A COINCIDENCE TEST, AND IT USED TO BE VACUOUS ON THE ONE FRONTIER IT MATTERS MOST ON.
           This compared the two BURNS alone, so on an un-charged frontier -- both zero -- it answered
           TRUE for any two buckets whatever, including two that are plainly different. That reads as
           "the minter IS the crowd", which hands a reader the fixture-scale refutation as though it
           covered the minter arm, which is the exact conflation the minter rows exist to end. An
           assert whose two sides cannot disagree is not a weak check, it is a NON-check that certifies
           whatever it was pointed at, and the early frontier is where a zero burn is likeliest.
           TWO REPAIRS, NEITHER OF WHICH WEAKENS IT. It now compares the three rows that IDENTIFY a
           bucket -- its live count, its lifetime mints and its receipt -- so two buckets standing at
           different sizes no longer read as one because neither has been charged. And where every
           operand is zero there is nothing to tell any two buckets apart, so it answers null rather
           than true: an unasked question and a measured agreement are different facts and this stream
           spells absence as null everywhere else.
           IT IS STILL A COINCIDENCE TEST AND NOT A PROOF OF IDENTITY, stated here so the next reader
           does not upgrade it: two distinct buckets may agree on all three rows by chance. What it
           can do is REFUTE -- a false is conclusive that the two selectors reached different arms. */
        {
          const ident = ["brCrowdLive", "brMinterLive", "brCrowdBornLife", "brBornLifeMax",
                         "brCrowdUsLife", "brMinterUsLife"];
          if (ident.some((k) => typeof w[k] !== "number")) out.selectorsAgree = null;
          else if (ident.every((k) => w[k] === 0))         out.selectorsAgree = null;
          else out.selectorsAgree = (w.brCrowdLive     === w.brMinterLive &&
                                     w.brCrowdBornLife === w.brBornLifeMax &&
                                     w.brCrowdUsLife   === w.brMinterUsLife);
        }
        /* SET FROM THIS CENSUS BEFORE THE LINE IS EMITTED, so the line where a run first separates carries
           its own index rather than the next one's.
           AND `brMinterGoneLife == 0` IS NOT THE DISCRIMINATOR, which is the row a reader reaches for and the
           wrong one. A shed count of zero is SUFFICIENT for the two selectors to pick one bucket and it is NOT
           NECESSARY: measured over the archive, 19 censuses carry `gone > 0` and 18 of them still agree, since
           a tie in `sub_born` can be broken the same way by both selectors. `selectorsAgree` above is the
           published form and the stronger one, and it is what this landmark is keyed on.
           WHAT THE ARCHIVE SAYS THE PAGE-SCALE ANSWER IS: UNOBSERVABLE, not `at par`. `departures` reads 0 at
           the terminal census AND at the maximum over every census in all 16 page-scale transcripts / 434
           censuses across three real sites, so `sub_gone` collapses the two selectors everywhere and the minter
           triple can say nothing the crowd rows do not. The question upstream of this one -- does anything on a
           real page's frontier ever depart -- is the blocker, and it is answered by `departures` and not here.
           AND THE CROWD PAIR IS A PROPERTY OF THE DOCUMENT, WHICH IS WHY A FIXTURE RATIO MAY NOT BE QUOTED AS A
           FACT ABOUT THE ENGINE. Re-derived at terminal from the archive with these same accessors: the smoke
           fixture reads 5e-05 .. 3.2e-04, a DIFFERENT fixture of the same engine in the same directory reads
           0.52 .. 0.86, and the page-scale runs read ~0.94 .. 1.04. Three orders of magnitude between two
           documents under one accessor. A fixture-scale crowd figure offered as evidence that the branch term
           demotes a branching arm is therefore evidence about that fixture; the smoke figures above disagree
           with a relayed 0.0027-0.0046 by an order of magnitude at an unestablished revision, which is a
           disagreement to RE-TAKE and not a refutation of either. */
        if (out.selectorsAgree === false && sepAt === null) sepAt = n;
        out.selectorsEverSeparatedAt = sepAt;
        out.selectorsCensusesSeen = n;
        /* CARRIED BESIDE EVERY RAW BURN ON THIS LINE, because result.c says a raw microsecond total is
           quoted with the quantum's unit and a quotient of two burns is not. */
        out.isCpu = (s.quantum && typeof s.quantum.isCpu === "boolean") ? s.quantum.isCpu : null;
        console.log(JSON.stringify(out));
      }
    }
  } finally { browser.disconnect(); }
}

main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
