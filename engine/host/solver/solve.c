/* @S solver — see solve.h. Forced-exec candidate: derive the breakout from the sink's lexical CONTEXT, inject
   it at the source, re-run the REAL code, and verify it FIRES. */
#include "solver/solve.h"
#include "solver/solve_html.h"
#include "solver/solve_js.h"
#include "solver/solve_filter.h"
#include "core/json_buf.h"
#include "core/frame/policy_container.h"
#include "core/html/trusted_types.h"
#include "core/dom/document.h"
#include "core/dom/node.h"             /* node_next_in — the one pre-order successor; see its own comment */
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "check.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */
#include "core/html/html_parse.h"      /* …and the ONE place one is parsed, which owns the tokens it produces */

enum { SINK_EVAL = 0, SINK_HTML = 1, SINK_URL = 2 };   /* JS / HTML / URL context -> different candidate set + fire oracle */

/* THE RUNNING FLOW's candidate mode. This was a file-scope global, which is only correct while one candidate
   runs start-to-finish with nothing else scheduled — the shape the verify driver has and the BFS does not.
   Reached through the running flow so a preemption cannot cross it. A NULL flow (baseline setup) has none, and
   the accessor says so rather than inventing a value. */
/* EVERY @S CANDIDATE FLOW SEEDED. A candidate RE-RUNS the page, so this number times the page's cost is most of
   what an @S search spends — and it is what says whether a run got slower because there were more searches or
   because each search grew. Reported beside the switch count for that reason: one number cannot decompose. */
static int g_cands_seeded;
int solve_candidate_count(void) { return g_cands_seeded; }
/* THE ARRIVAL CENSUS — what happened UPSTREAM of every entry this file emits, and the numbers that make an
   EMPTY @S surface readable. See detect_sink for the four states one empty array was the evidence for. They
   are three counters and not one because each answers a different question and only the triple is a reading:
   `reached` is sinks executed at all, `tainted` is how many of those arrivals carried attacker-controlled
   input, `suppressed` is how many of THOSE the unforgeable-principal rule declined to open a search for.
   THEY ARE READ TOGETHER OR NOT AT ALL, which is why one call fills all three rather than three accessors a
   caller could use one of. `tainted == 0` beside `reached == 0` and beside `reached == 4000` are opposite
   findings, and a consumer holding one number cannot tell which it has. */
static long g_sink_reached, g_sink_tainted, g_sink_suppressed;
void solve_arrival_census(long *reached, long *tainted, long *suppressed) {
    DCHECK(reached && tainted && suppressed,
           "the @S arrival census was asked for fewer than its three numbers — each is uninterpretable alone "
           "(see the counters' own declaration), so a caller taking one of them is about to report a state it "
           "cannot distinguish from its opposite");
    *reached = g_sink_reached; *tainted = g_sink_tainted; *suppressed = g_sink_suppressed;
}

static int  is_verifying(void)   { Flow *f = flow_running(); return f && f->cand_verifying; }

/* ONE CANDIDATE OF ONE SINK'S SEARCH — its bytes, WHAT KIND OF THING THOSE BYTES ARE, and how much of them has
   ever been seen at a sink. The three used to be a `char **` beside an `int *` beside a LEADING COUNT, and the
   kind is the member that makes the difference: a probe is an INSTRUMENT (an inert context probe, or a
   delivery probe built out of the source's own percent-encode set) and an escape is an ATTACK, and every
   reader of this list has to know which it is holding — the seeder must not withdraw an instrument, the
   derivation's hand-off assert must know whether an escape was received, the arrival assert must know whether
   the bytes that turned up could have been built by this search, and the report states the split.
   THE KIND IS A PROPERTY OF THE ENTRY AND NEVER OF ITS POSITION, which is what this field is FOR. It was
   `index < nprobe`, a positional convention re-established by one assignment (`nprobe = npl`) at one instant —
   correct only while that instant was provably the one moment nothing else had been pushed. That precondition
   is not a property of the label, it is a property of the ORDER two independent producers happen to arrive in,
   and §A-FIELD-A-CONSUMER-DEFAULTS' relative applies exactly: a count that means one thing when detection
   opens a search on a slot it created and another when it opens one a cold resume created is a field that
   means two things depending on who wrote it. Written by the pusher, which is the only party that knows.
   AND THE THREE TRAVEL TOGETHER FOR THE REASON THE OLD PAIR'S OWN COMMENT GAVE AND COULD ONLY HOPE FOR: `pl`
   and `surv_pl` were "parallel and grown together so the two cannot come apart", which is an obligation on
   every future push, free and clone site (§Architecture: a struct copied field-by-field must dup EVERY owned
   field). One array of one record is how that obligation stops existing. */
#define CAND_PROBE  1   /* an instrument — inert by construction, never an attack, never withdrawn */
#define CAND_ESCAPE 2   /* an attack — derived from a witness, or a single-context class's written-down vector */
typedef struct {
    char *bytes;   /* owned */
    int   kind;    /* CAND_PROBE | CAND_ESCAPE — see above */
    /* THE PER-CANDIDATE HALF OF THE SURVIVAL PAIR. `surv_run`/`surv_len` is the search's BEST and saturates at
       a full-length run the moment any one candidate lands intact — the ratchet's own consequence — so it
       cannot say whether the run it is reporting was the inert probe's or a breakout's. Those are the two
       flows whose only difference is these bytes, so telling them apart IS the remaining question. Report-only:
       the WFQ credit stays on the search-level ratchet, worth at most one rung, so this changes no ordering. */
    int   surv;
} CandPayload;

/* A detected sink awaiting fire-verification. `seeded` is per SINK, not per session: a sink discovered late —
   inside a lazily-imported chunk, inside an injected <script src> — is discovered after the frontier has already
   drained once, and a one-shot "the candidates are seeded" latch meant it never got any. That latch was a cap:
   it bounded verification by WHEN a sink was found rather than by whether it had been searched. */
/* `tried` is the COUNT of candidate runs this sink's search has had, not a bit: a sink with no PoC is REPORTED
   as a parked search, and "parked after 0 candidates" and "parked after 5" are different states of the search
   that a flag cannot tell apart. It counts RUNS and not breakouts, because for a derived-context sink the
   first of them is the CONTEXT PROBE, which re-runs the whole page exactly as a breakout does and is the run
   that produced every breakout after it — reporting it as free would say the search cost less than it did. */
/* …AND `reached` IS THE ONE THAT SAYS HOW FAR IT GOT, WHICH `tried` DOES NOT AND WAS BEING READ AS. `tried` is
   raised where a candidate is SEEDED, so it is fixed the moment the flow is created and says nothing about
   whether that flow ever executed: a search whose candidates are all still queued somewhere in the middle of
   the document reports the same number as one whose every candidate re-ran the page, arrived at the sink, and
   failed to break out. Those are opposite verdicts — the first is a document that has not been explored far
   enough, the second is a solver gap — and solve.h's own sentence ("how far the search got") was true of
   neither. That is the defaulted-field defect: a question whose answer is fixed at seed time, read as a
   measurement of progress.
   `reached` is raised at the ONE point that observes the answer — the candidate's own bytes arriving at its
   own sink — so `reached:0` beside `tried:N` says "no BREAKOUT of this search has re-executed as far as the
   sink yet" and nothing else can.
   IT COUNTS BREAKOUT ARRIVALS AND NOT PROBE ARRIVALS, and counting both is a mistake this field was
   introduced with — the same one-number-two-mechanisms defect it exists to end, committed inside the
   instrument built to end it. The context PROBE carries an inert locator and cannot fire by construction, so
   a search whose probe had arrived and whose derived breakout had not yet run reported `reached:1`,
   indistinguishable from a search whose breakout arrived and failed to break out. Those are the two readings
   the field was added to separate. Measured: a full-document run reported arrival for all five sink classes
   while only ONE of them — the URL class, which has no probe — was saying anything at all.
   THE PROBE'S ARRIVAL IS NOT LOST, because it was never this field's to carry: a derived class's breakout
   EXISTS only because the probe run returned one, so `tried >= 2` already states it and a second copy of one
   fact is a second copy that can be behind. The fire branch asserts that implication rather than restating
   it. */
/* `src` IS THE INJECTION IDENTITY AND `root` IS THE DELIVERY PROVENANCE, and this record held only the first
   while emit_delivery asked the registry with it. The registry is an exact strcmp over the DECLARED sources, so
   a derived identity — `{location.hash}.slice()`, which is what `location.hash.slice(1)` composes and what real
   code is written in — matched no row, and the envelope's silence was rendered as the positive statement it
   means for an undeclared source: "the engine declares no browser delivery for this source ... there is no
   navigation that reproduces it". That was printed under a fire-verified fragment XSS whose reproduction is one
   navigation, the one the researcher had just performed. The root is a fact about the VALUE, so it is read off
   the value at the detector (concolic_root_c) and carried here beside the identity, never re-derived from it. */
/* …AND THE TWO RUNGS THAT SIT BETWEEN THEM, WHICH IS THE WHOLE OF WHY A CANDIDATE NEVER GOT A SECOND TURN.
   §@S says the search is "DISTANCE-DIRECTED (a fitness of {filter-survived, sink-reached, context-escaped,
   handler-fires} the WFQ reads)". Two of those four had an observation site and BOTH of them are at or past
   the sink, so a candidate's reward was 0 for its entire runway and a flow that carried the attacker's bytes
   nine tenths of the way was worth exactly what an unstarted one was. flow_weight reads `val` and nothing
   else, and a candidate records no endpoints by design (endpoint_suppress), so "distance-directed" was true of
   nothing: measured on the smoke fixture, every @S candidate sat at reward 0 and the ordering reached each one
   about once.
   `surv_run`/`surv_len` ARE THE FIRST RUNG, and they are a FRACTION held as its two halves rather than a
   double, because the report has to be able to say WHICH numbers ("11 of 14 bytes") and a rounded ratio cannot.
   The pair is the BEST any candidate of this search has achieved, so the credit is a RATCHET on a distance:
   each improvement pays exactly the fraction it added and the whole rung is worth at most 1.0 across the
   search's entire life, however many times it is observed. That is the same "credit once at the crossing"
   `reached` uses, generalised from a boolean to a distance — a boolean has one crossing and a distance has as
   many as it has improvements, and neither can be re-earned. It needs no minimum run length (which would be a
   magic number): a coincidental short run in a string the candidate never touched can claim only its own
   fraction of ONE rung, exactly once, because the ratchet never pays for the same ground twice.
   `escaped` IS THE THIRD, and it is boolean per arrival like `reached`: the bytes are at an executable
   position or they are not. It is what separates the two failures `reached` reported identically — bytes that
   ARRIVED and bytes that GOT OUT — and until it existed the popup stated which of the two had happened from
   the sink CLASS, with nothing measuring it.
   ALL THREE COUNTERS HERE ARE THE LEDGER'S HALF AND NONE OF THEM IS THE COMPARATOR. They are facts about the
   SEARCH — how far the best of its candidates has ever got — and §@S(ii) is that such a record cannot order
   the search's own live candidates against each other, because a rung already reached is paid to nobody
   twice. The comparator's copy of the same three observations lives on the FLOW (flow.h's `cand_surv` and
   `cand_rung`, written beside each credit below), and the two are deliberately not one number: a ledger that
   could be re-earned reorders the frontier on repetition, and a comparator that cannot be re-read cannot say
   where anything stands.
   AND THE LADDER'S BOTTOM RUNG HAS NO COUNTER HERE, WHICH IS A DECISION. FLOW_RUNG_DELIVERED is the flow-side
   fitness that separates a candidate whose bytes never entered the program from one whose bytes a filter ate;
   as a LEDGER quantity it would be a 0->1 crossing paid to the first candidate of the search to reach its own
   source read and to none of the rest — §@S(ii)'s defect exactly, and the search learns nothing from the
   second delivery that it did not learn from the first. So the comparator carries it and the ledger does not.
   SO THE REPORT COUNTS THE TWO EVENTS THE RUNGS ARE OBSERVED AT, AND NEITHER COUNT IS A RUNG. `substituted`
   and `sink_strings` below are REPORT counters in exactly the sense `reached` and `escaped` are: nothing about
   the WFQ moves at either write, no crossing is latched and no ledger is paid, so §@S(ii)'s separation is
   untouched and the comparator on the flow stays the only thing that orders live candidates. What they add is
   the half a BEST-SO-FAR ratchet structurally cannot state — whether the observation was ever MADE. A ratchet
   reports the furthest anything got and is therefore silent about how many times it looked, so `surv_run:0`
   was two opposite readings under one number: no code-execution sink ran while this search's bytes were live,
   and N of them ran and not one byte of the candidate was in any of them. §@S names that tell exactly — a rung
   whose ABSENCE and whose ZERO read alike — and the two take opposite work, the first a distance question and
   the second a question about the payload's own transform.
   AND `substituted` IS THE ONE BELOW BOTH OF THEM, for the reason FLOW_RUNG_DELIVERED is the flow's bottom
   rung: it is observed at the SOURCE READ, in the component that performs the substitution, so `substituted:0`
   beside `turns:N` is the positive statement that these runs ended before reaching the read itself — a
   question about the PATH in front of the source, which is neither of the two above and which no field here
   could say at all. It is a count of substitutions and not of candidates: the flow-side rung already dedups
   per flow (flow.c's flow_observe_rung early-returns), and a second dedup here would need a latch this file
   does not own and an ordering contract between two components to keep it honest. */
typedef struct {
    char *src; char *root; int sink; int tried; int reached; int turns; int fires;
    /* HOW MANY TIMES THIS SEARCH'S BYTES HAVE ENTERED THE PAGE'S OWN PROGRAM — the report's bottom rung, and
       the lowest fact here whose observation site is a point in the page's OWN program. Written from
       solve_observe_substitution, which solver/concolic.c calls at the moment it performs the substitution, so
       it is strictly before every SINK-side number on this entry. It is no longer the only one that can be
       about a candidate still on the runway — `replay_pm` below is, and that is the whole reason it exists:
       this field's site is AT the source read, so `substituted:0` is where the runway BEGINS to be a question
       rather than a number that can answer one.
       `substituted:0` BESIDE `turns:N` IS A POSITIVE STATEMENT AND THE WHOLE REASON THE FIELD EXISTS: these
       candidates have held the thread and not one of them reached its own SOURCE READ, so the question is
       about the PATH in front of the source — a gate turning the flows away — and not about the payload, the
       filter or the sink. Without it that state was reported as `turns:N,reached:0,survived:0`, which is
       byte-identical to a candidate whose bytes DID enter the program and never reached a sink, and the popup
       stated the second for both: "the flows run and do not get this far through the document", which is a
       confident wrong instruction for a flow that never got as far as the read.
       A COUNT OF SUBSTITUTIONS, NOT OF CANDIDATES, and the name says so. A page that reads its source in a
       loop delivers many times in one run; deduping to distinct flows would need a per-flow latch this file
       does not own, and reaching for the flow's own rung to supply one would make the count depend on being
       written before flow_observe_rung — an ordering contract between two components, which is exactly the
       kind of seam that goes wrong silently. The zero is what is load-bearing here, and no dedup changes it.
       NOT A RUNG AND NOT A CREDIT. Nothing about the WFQ moves at the write: §@S(ii)'s ledger keeps its
       crossings, the flow's comparator keeps FLOW_RUNG_DELIVERED, and this is the REPORT's copy of the same
       event — the third accounting unit, and the only one a reader ever sees. */
    int substituted;
    /* …AND THE ONE RUNG BENEATH *THAT*, WHICH THE COMPARATOR HAS BEEN READING AND THE REPORT COULD NOT SAY.
       `substituted:0` beside `turns:N` is a positive statement — these runs ended before their own source
       read — and it is structurally silent about HOW FAR they got, which on a runway of hundreds of
       statements is the whole remaining question. The two states it has been saying at once are
         runway 0     — the candidates were given the thread and consumed none of their own recorded path:
                        a question about what is turning them back at the very start of the replay, and
                        nothing whatever about the distance to the source.
         runway ~1000 — they walked their recorded path and the source read is still in front of them: the
                        distance question, and the one the fitness rung below FLOW_RUNG_DELIVERED exists to
                        direct. Those take opposite work and were one number.
       THE OBSERVATION IS flow.c's AND IS NOT RE-DERIVED HERE. `Flow.cand_replay` is the fraction of a
       candidate's own recorded decision path that this run has REPLAYED, written by flow_observe_replay from
       dec_replay one arm at a time and asserted monotone there; flow_distance reads it as the ladder's bottom
       rung and flow_observe_rung PINS it to 1.0 at the delivery. Until now that was the whole of its
       readership — a value computed and asserted on every replayed arm and visible in no document, which
       §@S names as the mirror of the read-with-no-writer defect and which this file has already had to fix
       twice (`survivedAt`/`survivedTo`, and the derivations' own return values).
       THOUSANDTHS AND NOT A FRACTION, with the unit in the KEY. json_buf writes numbers through snprintf and
       every other count on this entry is an `int`; emitting a float here would put a locale-dependent
       spelling into a document three consumers parse. The unit is stated in the name for the reason
       CLAUDE.md gives about a counter's kind — a bare `runway` reads as a count of runways.
       A BEST-SO-FAR OVER THE SEARCH, like `surv_run`, because the record outlives every flow that produced
       it: the flow-side value is per FLOW and re-earned across a park, and what the card has to be able to
       say is the furthest ANY candidate of this search has been observed at. Nothing about the WFQ moves at
       the write — this is the REPORT's third accounting unit exactly as `substituted` is.
       WHAT IT IS A FRACTION OF IS DECISION ARMS AND NOT STATEMENTS, which is the reading a consumer must not
       get wrong: a candidate whose recorded path is short saturates this at 1000 while still far from its
       source read in program order. That is not a defect in the number, it is the same fact about the
       comparator — so `runwayPerMille:1000` standing beside `substituted:0` says the ladder's bottom rung is
       SATURATED and the remaining runway is unmeasured by it, which is §@S(i)'s objection one level out and
       is exactly what this field exists to be able to state.

       AND `0` IS TWO STATES, NOT ONE, WHICH IS THIS RUNG'S OWN OBJECTION ARRIVING ONE LEVEL DOWN. The reading
       above names 0 as "given the thread and consumed NONE of their own recorded path" — a question about
       what turns a replay back at its FIRST ARM. That presumes an arm was offered. `flow_observe_replay` is
       called from ONE line (decide.c, the only line in the engine that CONSUMES a recorded arm) and only for
       a flow carrying a payload, so a candidate that never consumes one never reaches the observation site at
       all and this field stays at its initial 0. "No arm was ever consumed" and "arm 0 was refused" are
       therefore byte-identical here, and they take opposite work: the first is a question about whether this
       search ever had a recorded path to walk, the second about what stands in front of its first branch.
       That is §@S(i)'s tell — a rung whose ABSENCE and whose ZERO read alike — which is the defect the runway
       was added to END for `substituted`, reproduced in the instrument that ended it.
       MEASURED ACROSS 76 SMOKE LOGS AND THE SPLIT IS BY SOURCE, NOT BY SINK CLASS. Every parked record whose
       source is `location.hash` or a derivative of it (`{location.hash}.slice(1)`) reads 0 here in EVERY run
       that produced one — 1497 records over 51 runs, zero readings — while every `{state}.*` source reaches a
       nonzero runway in some run. `eval` and `innerHTML` each appear on BOTH sides of that split, so it is
       not a property of the sink class; the fixture's `s-loc`, `s-attr` and `s-park` rows never reach this
       rung because all three are bound to `location.hash`; `s-attr`'s ATTR_SRC is `{location.hash}.slice(1)`.
       WHAT SEPARATES THEM IS A DENOMINATOR AND IT IS BUILT — `reinject_len` below, emitted as `runwayArms`.
       The two are read together: `runwayArms:0` is the search having no recorded path to replay at all, and
       `runwayArms:N` beside a 0 here is the first-arm reading this comment describes.
       THE NEXT-DIFF CLAUSE THAT STOOD HERE NAMED A REACHED BIT — "a flag set at the first
       flow_observe_replay for this candidate, emitted beside this number" — AND IT COULD NOT HAVE SEPARATED
       THE TWO STATES THE PARAGRAPH ABOVE IT NAMES. It is recorded rather than deleted because its METHOD is
       the reusable part: it was written from the OBSERVATION SITE, and both of those states are states in
       which that site is never reached. `flow_observe_replay` is called from dec_replay on the far side of
       `g_c++`, so a key mismatch at slot 0 returns -1 without ever reaching it, and a search with no recorded
       path never enters dec_replay at all — decide_branch guards the whole arm with `g_c < dec_total()`. The
       flag therefore reads "never observed" for BOTH, and true exactly when at least one arm was consumed,
       which is what a nonzero here already says; its only marginal reading is the rounding case named below,
       the least valuable of the three splits. THE TELL WAS IN THE SENTENCE DIRECTLY ABOVE IT, which names the
       DENOMINATOR as the thing that would separate them and then prescribes something that is not one — a
       remedy clause disagreeing with its own diagnosis inside one paragraph.
       AND THE ROUNDING THAT LEFT IS BUILT TOO — `replay_arms`/`replay_of` below, emitted as `runwayWalked`
       and `runwayOf`. This conversion is `(int)(cand_replay*1000.0+0.5)`, which is 0 for any path longer than
       2000 arms with one arm consumed while decide.h records replay depths of 8000, so a 0 here was ALSO
       "arms were consumed and the fraction rounded away". The numerator is now carried as a COUNT from the one
       place dec_replay's own note says it is honest — flow_observe_replay's `consumed`, honest at that line
       "and not at the scheduler" because the cursor also advances on appends — so `runwayWalked:3` beside a 0
       here is the rounding and `runwayWalked:0` beside it is a replay that took no arm.
       THIS FIELD IS THEREFORE NO LONGER THE PRIMARY READING OF THE RUNG AND IS KEPT, WHICH IS A CHOICE. It is
       not a second copy of the pair: it is the COMPARATOR's best, so it carries flow_observe_rung's delivery
       PIN — a policy, by that function's own words "BY DEFINITION AND NOT BY OBSERVATION" — while the pair is
       only ever an observation. Below the delivery the two reconcile exactly; past it they differ, and each
       says which of the two questions it is answering. */
    int replay_pm;
    /* …AND HOW MANY ARMS THE PATH THAT FRACTION IS OF ACTUALLY HAS, WHICH IS WHAT SPLITS THE ZERO ABOVE.
       The re-injection point is frozen ONCE per search — add_pending, under the `opened` latch, at the one
       moment a flow stands at this sink — and every candidate is seeded at cursor 0 over that same frozen
       segment, so the segment's LENGTH is a property of the SEARCH: fixed from the freeze, known before any
       candidate is seeded or scheduled, and already named by the accessor that reports it (decide.h: "the
       length of the CHAIN ... fixed from the instant a blob is built and is the DENOMINATOR of any question
       about progress").
       `runwayArms:0`   — the detecting flow decided NO branch on its way to this sink, so there is nothing
                          for a candidate to replay. Every candidate then begins on an empty vector and forks
                          the document's own gate tree exactly as the design before the re-injection point did
                          (the measurement at the capture: 2049 flows for one sink at K=10), and the 0 above
                          is a tautology rather than an observation. A question about the DETECTION.
       `runwayArms:N` beside `runwayPerMille:0` — N arms were offered to every candidate of this search and
                          not one was consumed: what stands in front of the replay's FIRST branch. Those two
                          take opposite work and were one number.
       IT IS NOT `runwayPerMille`'s DENOMINATOR AND THE TWO DO NOT MULTIPLY, which is why it is not named
       `runwayOf` after the `survived`/`survivedOf` pair it otherwise resembles. That fraction is a best-so-far
       over samples and a sample's own denominator is `dec_total()` at the instant dec_replay took it — which
       for an ARM forked off a candidate includes the slots its own fork appended, so it can exceed this
       number. What this states is where every candidate STARTS; an arms-consumed count is a different field
       and is not derivable from these two.
       STORED AT THE FREEZE AND NOT READ OFF THE BLOB AT EMISSION, and that is not a convenience: record_sink
       releases `reinject` the moment the search FIRES, so a length derived at the emitter would read 0 for
       every search that succeeded — the same absence-and-zero defect one field over, pointing in the
       direction that reports the searches which WORKED as the ones that had no path.
       A SIZE AND NEVER A DISTANCE. decide.h keeps that pair apart by hand for exactly this reason — the
       cursor is the position, `entries` is the length — and records what conflating them cost once already:
       flow.h's `cand_dec_max` was fed from the LENGTH under a contract calling it how far the best of them
       had GOT. This is the length, and the name says arms and not progress.
       ITS CONSUMER IS THE CARD AND IS DEFERRED, WHICH IS WHY THE FIELD GATE REPORTS THIS AS A WRITE WITH NO
       READER AND IS RIGHT TO. Every sibling on this entry is read by extension/lib/popup-security.js and
       required by lib/store-record.js's currency predicate; these three are in neither yet, because that JS
       deploys on WRITE while this C is live only after a build (§A-CROSS-BOUNDARY-DIFF) and the currency
       predicate SHEDS what it judges stale — so requiring them early would drop every parked @S record in the
       store until the next install. THE OBSERVATION THAT RETIRES IT, and not the reason: these names occurring
       in `extension/lib/qjs/qjs.wasm`, checked by CONTENT with `runwayPerMille` as the positive control and an
       invented name as the negative one. Measured at this commit: the control PRESENT, all three ABSENT. When
       the artifact carries them the reader and the requirement land together, in one diff. */
    int reinject_len;
    /* …AND THE POSITION ITSELF, HELD AS ITS TWO HALVES RATHER THAN AS THE THOUSANDTHS ABOVE — the same repair
       `surv_run`/`surv_len` is one rung up, and for the same stated reason: the report has to be able to say
       WHICH numbers, and a rounded ratio cannot. `replay_pm`'s conversion is 0 below one part in two thousand
       and decide.h records replay depths of 8000, so the rung's own zero was still two states after
       `runwayArms` split off the third.
       ITS RATCHET IS THE PAIR'S OWN AND IS CROSS-MULTIPLIED, not a second reading of `replay_pm`. Ratcheting
       on the rounded value is exactly what cannot record the case this pair exists for — a best fraction of
       3/8000 rounds to 0, never exceeds a `replay_pm` of 0, and the sample is dropped. Integer throughout, so
       no float enters this struct and none is emitted.
       `replay_of` IS `dec_total()` AT THAT SAMPLE AND IS NOT `reinject_len`. The frozen path is what every
       candidate STARTS on; the denominator of a reading is the vector as it stood when dec_replay took it,
       which for an ARM forked off a candidate includes the slots its own fork appended. The two are emitted as
       different keys because they answer different questions and a reader who took one for the other would
       compute a progress along a path that flow never stood on.
       0 IS "NO READING WAS EVER TAKEN" AND IS THE LOAD-BEARING VALUE, exactly as for the two fields above:
       flow.h's pair carries that meaning in `of` and asserts it as a biconditional with the fraction, so an
       `of` of 0 here is that statement travelling rather than a hole the ratchet never filled. */
    long replay_arms, replay_of;
    /* …AND HOW MANY STRINGS A CODE-EXECUTION SINK WAS HANDED WHILE THEY WERE LIVE — the observation COUNT that
       `surv_run` is the best of, and the half a ratchet cannot state. A best-so-far records the furthest
       anything got and says nothing about how often it looked, so `surv_run:0` meant either "no sink ran at
       all during a candidate run of this search" or "N of them ran and not one byte of the candidate was in
       any of them". Those are §@S's ABSENCE and ZERO reading alike, and they take opposite work: the first is
       a distance-through-the-document question, the second says the sinks EXECUTED and the page's own
       transform or routing left nothing of the payload in what they executed.
       CLASS-INDEPENDENT, exactly like the fraction it counts the observations of (filter_survived's own
       comment): the question is whether ANY code-execution sink ran with this flow's substitution live, so a
       candidate for the eval sink whose bytes turn up at a markup write is counted here. Attributed to the
       RUNNING FLOW'S search and never to the sink's, for the same reason the fraction is.
       A COUNT OF STRINGS AND NOT OF ARRIVALS — `reached` is the arrival of a BREAKOUT at its OWN sink, and
       this counts every string any code-execution sink was handed, whether or not a byte of the candidate was
       in it. Those differ by exactly the population the field exists to describe. */
    int sink_strings;
    /* THE LEDGER'S OWN LATCH FOR THE ARRIVAL RUNG, AND IT IS SPLIT OFF `reached` BECAUSE ONE NUMBER WAS
       ANSWERING TWO QUESTIONS AND ONE OF THE ANSWERS STOPPED BEING TRUE. `reached` is a REPORT counter — how
       many times ANY breakout's bytes turned up at this sink — and it was ALSO the WFQ's 0->1 crossing. Those
       coincide only while every queued breakout is one the search still holds as viable, and they do not: the
       delivery table is measured AFTER a breakout is queued, so a spelling this search's own observation has
       since contradicted still arrives, still raises `reached`, and took the whole rung with it — leaving
       nothing for the spelling that can actually fire.
       THE ARRIVAL IS REAL AND STAYS COUNTED; what the LEDGER pays for is the observation the rung NAMES. The
       rung is a distance to FIRING, so "a breakout reached this sink" is worth a point because it is progress
       TOWARD a fire; a candidate whose bytes this search has measured cannot arrive intact has made no such
       progress, and paying it is paying for ground the search already knows leads nowhere. */
    int reach_credited;
    int surv_run, surv_len;
    /* …AND WHERE THAT RUN IS, WHICH IS THE HALF THE PAIR CANNOT SAY AND THE HALF A MUTATION ACTS ON.
       `surv_run`/`surv_len` is HOW MUCH of a candidate survived; these two are WHICH PART. `surv_at` is the
       offset into the CANDIDATE at which the surviving run begins — so the rest of the candidate is the
       segment that died — and `surv_out` is where that run was found IN THE STRING the sink was handed. They
       are §@S(2)'s two named facts read literally ("which bytes survive to which positions"), and §@S names
       them again as the input to the step that follows: "a near-miss is mutated toward the gap using the
       byte-provenance the run already measured — which segment died and where the rest landed".
       THEY WERE COMPUTED, VERIFIED, AND THROWN AWAY. solve_filter_survival reports both on every observation
       and its own two-sided assert RE-READS the bytes at them, so the numbers were real and checked; they
       then died with the `FilterObs` local in filter_survived, which reads `run` and `len` and nothing else.
       That is §@S's closing sentence exactly — "an observation with a computed writer and no reader is not a
       mechanism" — and it is the mirror of the read-with-no-writer defect this file has fixed twice
       (`witnessed`, and the derivation's own return count): harder to see, because nothing is missing, nothing
       defaults, and the value is asserted on the way to nowhere.
       WHAT THE READER GAINS IS A DIRECTION AND NOT A DETAIL. `survived:11,survivedOf:14` says three bytes died
       and cannot say which three, and the two answers take OPPOSITE work: a run at offset 0 is a payload whose
       TAIL the page cut — the escape opened and never closed — and a run at offset 3 is one whose HEAD it ate,
       so the escape never opened at all. The size of the gap was reported and its position was not, so the two
       read alike in the field built to tell distances apart.
       THEY DESCRIBE THE RATCHET'S RECORDED RUN AND NOTHING ELSE, so they are written in its improvement branch
       and never on their own: offsets from one observation beside a length from another would name a segment
       of a string no candidate ever produced. -1 is the no-observation state — the same one FilterObs uses,
       for the same reason — and it is why the report OMITS them rather than emitting 0, since offset 0 is a
       real answer and the most common one. */
    int surv_at, surv_out;
    int escaped;
    /* …AND THE SAME LATCH ONE RUNG UP, WHICH IS THE SENTENCE ABOVE READ AGAIN AT THE RUNG IT WAS NOT READ AT.
       `escaped` is the ESCAPE rung's report counter for exactly the reason `reached` is the arrival rung's —
       every arrival at an executable position is counted, including one by a spelling the delivery table has
       since contradicted, because those bytes really did stand there — and it was ALSO that rung's 0->1
       crossing, which is the pair of jobs one number cannot hold. The failure is the arrival rung's verbatim
       and one rung more expensive: the first contradicted escape raises `escaped` to 1 without being paid
       (the credit sits inside the deliverability gate and the count sits outside it), and the spelling that
       CAN fire then escapes into a latch already spent, so the search is never credited for the last rung
       before a fire. A ledger that can be spent by an observation it refused to pay for is not a ledger.
       WHY THE COUNT STAYS OUTSIDE THE GATE. Moving it inside would make `escaped` a claim about VIABILITY
       rather than a count of arrivals — the same wrong fix `reached` was offered — and would delete the one
       report that says a contradicted spelling reached an executable position at all. */
    int escape_credited;
    /* THE BREAKOUTS THIS SINK'S SEARCH HAS, IN ORDER, AND HOW MANY OF THEM ARE ALREADY FLOWS. A derived-context
       sink does not KNOW its breakouts when it is detected — a probe run reads them off the sink's own parse —
       so the list GROWS after seeding has already happened, and a one-shot "this sink is seeded" latch cannot
       express that. A cursor can: seeding takes everything past it, so a breakout derived at any later moment
       is picked up by the next drain and none is ever seeded twice.
       TEXT, never an index into the class table: a candidate flow carries its payload to the cold tier and back
       (cold.c parks `cand_payload` as bytes for exactly this reason), so a recipe parked this session must
       still mean the same thing to a build whose tables have changed. */
    CandPayload *pl; int npl, plcap, seeded;
    /* HOW MANY OF THIS SEARCH'S RUNS CAME BACK OUT OF THE COLD TIER — the operand solve.h's own arithmetic for
       `tried` names and nothing emitted, which made that arithmetic unperformable by the only reader it was
       written for. `tried` is "the entries not marked `withdrawn`, PLUS any candidate resumed out of the cold
       tier, which has no row at all", and the second term was a sentence rather than a number: a reader
       holding `tried:6` beside an empty `payloads` could not tell a cross-session search from a producer that
       had dropped a field. Raised beside `tried` at the one door that produces it (solve_resume_candidate).
       IT IS ALSO WHAT MAKES `reached` READABLE BESIDE A PROBES-ONLY LIST. "A search holding nothing but its
       own probes cannot have had a breakout arrive" is TRUE of a search all of whose runs are rows in that
       list and FALSE the moment one of its runs is a resumed candidate, whose marker-carrying bytes ride the
       FLOW and are in no row — the same implication solve.c's arrival assert used to make and the popup still
       makes. Emitted so the consumer states it from a producer fact instead of inferring it from an
       arithmetic over three other fields, which is the view-restating-a-producer-fact this file declines
       twice elsewhere. */
    int resumed;
    /* …AND HOW MANY CAME BACK AND WERE REFUSED, which is a THIRD state and not the absence of the second. A
       parked candidate carries bytes a PREVIOUS session's derivation constructed, and this session's rules are
       not that session's: the root's carrier declaration narrows the delivery table the moment the root
       arrives (cand_learn_root), so a record written before that narrowing names a payload whose arrival this
       build can positively contradict. solve_resume_candidate withdraws it, and this is where that says so.
       IT IS NOT `resumed` MINUS ANYTHING AND IT IS NOT `tried`: a withdrawn record never becomes a candidate
       run, so neither of those may move for it, and without this field the three states — no candidate was
       ever parked for this search, one was parked and ran, one was parked and this build refuses it — collapse
       into the one answer `tried:0,resumed:0`, which solve.h defines as the positive statement that every run
       this search has had is a row in `payloads`. That is the same silent-wrong-verdict shape `opened` was
       made a field to end, arriving one door over.
       IT IS ALSO THE EVIDENCE add_pending READS. A withdrawal creates the array slot (the table has to exist
       before it can refuse anything) and raises nothing else, so an entry standing at `tried:0` is no longer
       proof of a third writer into g_pending — this number is what tells the two apart. */
    int resumed_withdrawn;
    /* HAS DETECTION OPENED THIS SEARCH — a fact about the SEARCH, and the reason it is a field rather than the
       `created` flag add_pending used to read. Two doors reach an entry and only one of them can open a
       search: DETECTION stands at the sink holding the value that arrived, which is what the class's probe and
       the re-injection point are both taken from, while a COLD RESUME merely re-registers a candidate of a
       search a previous session opened. `created` answers "did this call make the array slot", and the two
       questions come apart in the order the cold tier makes structural — cold_resume runs at engine init, so
       in a resuming session the resume ALWAYS makes the slot and detection always found one. Read off
       `created`, detection then returned before pushing anything: no probe, no vectors, no path, and a report
       that said `payloads:[]`, which solve.h defines as the positive statement that this source can carry no
       exit at all. See add_pending. */
    int opened;
    /* WHICH BYTES OF A CANDIDATE ACTUALLY REACH THIS SINK, and the witness a changed answer is re-derived from
       — §@S's SECOND observation, which the derivation was being run without.
       §@S requires the three observations to be solved JOINTLY: the sink's parse context, the per-flow
       character provenance, and the path's value domain. solve_html.c had the first and constructed
       `'><svg onload=X9()>` out of it; the fragment percent-encode set (URL §1.3 "Percent-encoded bytes")
       holds SPACE, `<` and `>`, so those bytes arrive as `%20%3C%3E` and that escape is unsatisfiable BY
       CONSTRUCTION. The search could not say so — it reported an escape that merely did not fire, beside a
       `survivedBy` that measured the gap and fed nothing.
       IT IS MEASURED AND NOT READ OFF THE DECLARATION, which is the whole reason it is a table on the search
       rather than a call to concolic_source_encodes at the derivation. A page that runs `decodeURIComponent`
       over its own fragment receives the `<` the browser encoded, and this engine already FIRES a markup PoC
       through exactly that round trip; taking the declaration as the constraint would decline it. What the
       declaration decides is which bytes are worth ASKING about — the delivery probe is built out of exactly
       that set — and what the run decides is the answer.
       THE WITNESS IS WHAT MAKES A CHANGED ANSWER A MUTATION RATHER THAN A NEW PROBE. It is the string the
       context probe's own run handed this sink, so re-deriving from it under the tightened table is §@S's
       "a near-miss is mutated toward the gap using byte-provenance" performed on the observation already
       taken: no second probe, no retry counter, and what it constructs joins THIS search so the next drain
       seeds it as an ordinary flow of the one frontier.
       THE WITNESSES ARE A LIST FOR THE REASON `pl` IS. A page writes one source into a sink as often as it
       likes — a loop over innerHTML, a template rendered per row — and each write is its OWN string with its
       own contexts, all of them this one search's. Keeping the latest would make a re-derivation answer for
       whichever write happened last, which is the single-slot defect `reached` and `survivedBy` were each
       split out of. Deduped by text, because two writes of the same template produce the same witness and
       re-deriving it twice can only produce breakouts push_breakout already holds. */
    /* AND WHETHER THE TABLE IS A MEASUREMENT YET, which is not the same question as what it holds and is not
       answerable from it. The table starts permissive and is narrowed by TWO things that are not the same kind
       of fact — a run (observe_delivery) and the root's own carrier declaration (cand_learn_root) — so its
       contents alone cannot say whether anybody has been asked anything: "every byte arrives", "no delivery
       probe has run" and "the carrier refuses bytes no escape here happens to carry" are three states one
       reading cannot separate. That is the defaulted-field defect exactly, and a permissive table read as an
       observation would report a page that decodes its own fragment for one that has never been asked. This
       latch is raised ONLY by a run, the report emits the measured set only when it says there is one, and its
       absence is the positive statement that none was taken. */
    /* …AND HOW MANY TIMES THE DELIVERY PROBE ITSELF REACHED A SINK, WHICH IS A DIFFERENT FACT FROM THE ONE
       ABOVE AND WAS READING AS THE SAME ONE. `deliv_seen` is raised only where a probe TOKEN was found in the
       output, so a probe that arrived and whose every token the page destroyed leaves it at 0 — and
       cand_delivers, which gates the report on it, then emits nothing, exactly as it does for a probe that has
       never run. Those are opposite findings: the second is a search waiting on the scheduler, the FIRST is a
       page whose own transform eats the INSTRUMENT, which is the strongest evidence a parked search can have
       and is the state a derivation must stop constructing against. The tell §@S names, at the rung directly
       above `turns`, inside the instrument built to end it.
       IT IS COUNTED AND NOT GATED ON, so the fix adds a fact and takes none away: `deliv_seen` still decides
       whether a MEASURED SET exists, and it must, because a table narrowed by nothing is the permissive one and
       emitting it would state "every declared byte arrives" about a run that observed no byte at all — the
       defaulted-field defect, one field over, in the direction that fabricates. */
    SolveDelivered deliv; int deliv_seen; int deliv_runs; char **wit; int nwit, witcap;
    /* THE SEARCH'S RE-INJECTION POINT — the decision state the DETECTING flow stood on when the attacker value
       reached this sink, held so that EVERY candidate of this search REPLAYS that path instead of searching for
       it again from nothing. One capture, at add_pending; queue_derived asserts it rather than taking a second.
       WHY EVERY CANDIDATE AND NOT ONLY THE DERIVED ONES. A candidate with no path must find, among a fork tree
       as deep as the document's gate sequence, the one arm that reaches the sink — and it pays that in full
       whatever seeded it. Measured on the shipped artifact with K independent concolic gates in front of one
       sink: the exploration is 2^K flows and each pathless candidate re-forks all 2^K of them. The claim that
       used to stand here — that a single-context class is exempt because it "states its vectors at DETECTION,
       so its candidates are seeded when the frontier is small" — is true about WHEN and false about COST: at
       K=8 the URL sink created 2^8 + 2 x 2^8 flows, because it has two written-down vectors and neither had a
       path. A fresh flow's whole advantage is its optimism bonus, 1/(1+n), which outranks a saturated frontier
       for about twelve quanta and then ties with everything; twelve quanta does not buy a traversal.
       A FLOW IS `replay(baseline, decision vector)` (flow.h), which is why this is a blob and not a frame: the
       cold tier already rebuilds a parked CANDIDATE from nothing but its vector, so a candidate flow standing
       on a vector it did not itself run is not a new capability, it is the one cold_resume performs every time
       it brings one back. decide_freeze_path is the capture — it freezes the running head and hands back a blob
       at the flow's CURRENT cursor with no frontier member behind it, which is exactly what a flow standing at
       a sink it has just detected is.
       IT IS SOUND UNDER A DIFFERENT PAYLOAD because the substituted source is CONCRETE during a candidate run:
       branches on it are decided by running the real predicate on the real value, never from the vector, so a
       breakout whose bytes take a different arm takes it. The vector only replays the arms the payload does
       not decide, which is the definition of "the same world, different attacker bytes".
       IT IS A CEILING AND IT IS RELEASED. The blob holds a reference on the frozen segment for the life of the
       search, so the whole prefix under it stays alive; record_sink drops it the moment the search fires and
       solve_free drops what is left. */
    void *reinject;
} Cand;
static Cand *g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;

/* THE SINK CLASS TABLE, defined below its candidate sets. Everything a report says about a sink is a row of
   it, so these two are how the rest of the file reaches one. */
typedef struct SinkClass SinkClass;
static const SinkClass *sink_class(int sink);
static const char      *sink_name(int sink);
static int              sink_class_of_name(const char *name);
/* …AND THE STORE, declared here for the ONE caller that stands above it: the fire marker. The marker is
   installed by solve_init, which is above the table, and it is where a finding is now MADE — so the
   declaration goes where the table's already are rather than the marker being moved below the store and
   solve_init below that. */
static void record_sink(int cls, const char *source, const char *poc);

/* A FIRE-VERIFIED PoC. The sink is held as its CLASS, not as its display name: every fact the reproduction
   envelope states — the CSP question, the Trusted Types question, what makes the breakout run — is a fact
   about the class, and holding the name meant asking for them with a `strcmp` chain over display text at emit
   time. That chain computed the CSP question and threw the other two away. */
typedef struct { int cls; char *source; char *root; char *poc; } Finding;   /* verified PoCs only */
static Finding *g_sinks = NULL; static int g_sinks_n = 0, g_sinks_cap = 0;

/* THE MARKER, AND THE FINDING IS MADE HERE BECAUSE THIS IS WHERE THE PROOF HAPPENS. §@S: only FIRING proves a
   PoC — X9 actually CALLING is the whole of the oracle — so the instant this runs there is nothing left to
   wait for and nothing left to check.
   IT USED TO SET A FLAG that solve_flow_end turned into a finding at FLOW_STEP_DONE, and that made an emitted
   PoC conditional on the flow REACHING COMPLETION — which §NO BOUNDS says no flow owes anybody, and which
   this engine already breaks in two more ways on purpose: cold.c drops `cand_fired` on a park (deliberately,
   so a resumed candidate re-proves itself), and flow_remove frees a SOLD flow's substitution without passing
   through solve_flow_end at all. Three ways to observe the proof and discard it, none of which said anything —
   a candidate that fired and then kept exploring, parked, or was paged out reported as a search that had not
   solved, which is the one verdict §@S forbids being arrived at by omission.
   A MARKER OUTSIDE A CANDIDATE FLOW IS THE PAGE'S OWN CALL, and that is a partition rather than a swallowed
   error: X9 is a global the analysed bundle can reach, and a page calling it has proved nothing about a
   substitution nobody made. It is the same distinction solve_eval_sink makes on the value it is handed. */
static JSValue js_x9(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    Flow *f = flow_running();

    (void)ctx; (void)t; (void)c; (void)v;
    if (!f || !f->cand_src) return JS_UNDEFINED;   /* the page's own call — no substitution, nothing proved */
    DCHECK(f->cand_payload && f->cand_sink,
           "an @S candidate flow fired the marker holding a source but no payload or no sink class — a finding "
           "IS that triple, so a flow carrying half of it was assembled somewhere that does not go through the "
           "seeder or the cold tier's rebuild");
    record_sink(sink_class_of_name(f->cand_sink), f->cand_src, f->cand_payload);
    return JS_UNDEFINED;
}

/* THE FIRE. A sink executes attacker-shaped code — `eval(s)`, a `javascript:` navigation, an auto-firing event
   handler in re-parsed HTML — and that code is the PAGE's, so it can hold a loop, an await, a recursion. Running
   it with JS_Eval from C entered a bytecode body below the live candidate flow, where it cannot suspend: the
   engine's own DFAIL named it a drive-to-completion, and the whole @S verification was built on one.
   The sunk code is simply MORE CODE IN THIS FLOW — the same thing a lazy chunk is — so it is queued as another
   program of the running flow and the ONE BFS runs it, preemptible and parkable like every other. The candidate
   re-run drains its queue before finishing, so the flow's own fire flag still answers when it completes. */
/* AND WHERE IN THAT FLOW'S SEQUENCE IT GOES IS THE SINK'S OWN SEMANTICS, which is why it is a parameter rather
   than one answer for all three. §@S already says the firing vector is chosen per sink from its real
   semantics; the POSITION is part of that vector and was the half nobody stated. It went to the tail for every
   sink, so a PoC this search had already CONSTRUCTED could only be proved after the flow had run every
   remaining program of the document — which puts the proof back behind flow completion, the exact dependency
   js_x9's own comment above records as removed. The marker still records the finding the instant it runs; this
   is about when it gets to run. */
/* AND THE PAYLOAD CROSSES AS `(src, len)`, WHICH IS THE PAIR THIS FUNCTION ALREADY HELD. It used to malloc a
   NUL-TERMINATED copy and hand the queue only the pointer — so a candidate the search had built with a U+0000
   in it (a `%00` percent-decoded out of a hash, a NUL a JSON reply carried) was fired as its PREFIX, and the
   "did not fire" that followed was a verdict about a program nobody chose. The copy is gone with it: the queue
   copies into the shared body it makes (solver/dyn_body.h), so the temporary was a second copy of the payload
   whose only job was to carry a terminator the queue writes for itself. */
static void fire_js(const char *src, size_t len, DynPos pos) {
    engine_queue_candidate(src, len, pos);
}

/* THE BREAKOUTS A SINK CLASS STARTS ITS SEARCH FROM, where they are WRITTEN DOWN rather than derived.
   §@S allows exactly one reason to write one down, and CANDS_URL is the only list left in this file because it
   is the only sink that meets it: the sink IS a single context, so there is nothing to derive — navigating a
   URL executes the `javascript:` scheme and nothing else does. Its row says which context makes it the only
   answer.
   CANDS_HTML AND CANDS_JS ARE BOTH GONE, and they went for one reason. Five guessed markup payloads at every
   markup sink and five guessed JS payloads at every eval sink: a sink whose lexical state none of the five
   fitted was unsolvable BY CONSTRUCTION and the search could not say so — it reported `parked, tried 5` while
   never once stating what it had failed to escape. Two of the JS five could not fit ANY state ECMAScript §12
   defines: a backtick closes a template whose own terminator is still ahead in the source, and `${X9()}` was
   sprayed at strings and comments that have no substitution while the state that does have one got no
   candidate of its own. solve_html.c reads its context off the REAL parse of the sink's OWN output and
   solve_js.c scans the eval sink's OWN argument per §12; each CRASHES on a state it cannot name, which is the
   difference between a search that has not solved and a capability that is not built. */
static const char *CANDS_URL[] = {
    "javascript:X9()",     /* URL context: the vector IS the javascript: scheme (one fixed context) */
    "javascript:X9()//",
    NULL
};

/* HOW A SINK CLASS GETS ITS BREAKOUT. This is ROUTING between two GENUINELY DIFFERENT algorithms and not a
   fallback selecting against a legacy body — the test §C-stack states: delete the derivation and a
   single-context sink still needs its one stated vector, delete the vector list and a derived sink still needs
   its parser, so neither is the other's leftover. A class declares exactly ONE of the two and solve_init
   asserts it, which is what stops a class being added with neither and reported as parked forever. */
enum { SINK_DERIVE_NONE = 0, SINK_DERIVE_HTML, SINK_DERIVE_JS };
/* THE SINK CLASSES — one row per lexical context the solver breaks out of, and the row holds everything that
   is a fact about THE SINK rather than about a run. They are one row because they are one thing: the sink's
   FIRE ORACLE. Before this they were four scattered statements of it, and one of the four did not exist:
   the breakout set was a function keyed by the class, the CSP question was a `strcmp` chain over the class's
   DISPLAY NAME written at emit time, the Trusted Types question was asked by nobody at all though
   trusted_types.c answers it, and what makes a fired breakout actually RUN was never stated anywhere — so a
   reader of a PoC could not tell one that fires at parse time from one that needs a navigation, which §S(d)
   requires every emitted PoC to carry.
   `fires_on` IS THE ORACLE'S OWN SEMANTICS, read off the oracle rather than chosen beside it:
     - the eval sink IS its own oracle: 19.2.1 and 20.2.1.1.1 announce their source to this file and then
       COMPILE AND RUN it on the flow's own tramp chain, so a fired eval PoC runs the instant the page reaches
       that call, in the scope and strictness the spec gives it. Nothing here models that evaluation — a
       modelled one beside the real one is a second executor, and the one that stood here was the weaker of
       the two;
     - html_fire_walk runs `onload` and `onerror` and NOTHING else (the AUTO-firing handlers — `onmouseover`
       needs interaction) over markup it has just parsed, and solve_html.c's constructed escapes end in an
       auto-firing element for exactly that reason, so a fired HTML PoC runs at insertion and never needs a
       click;
     - url_fire runs a URL's JS only when the scheme is `javascript:`, which is code that runs when the
       NAVIGATION happens — a Location assignment on the line that makes it, a form's action on submission.
       AND WHERE THE HOST PERFORMS THAT NAVIGATION ITSELF, ITS OWN §7.4.2.2 "Beginning navigation" step 16
       EVALUATES THE PROGRAM TOO, so the marker is reached twice for one candidate. That is not the double
       execution §Architecture forbids and it is not a fixture artifact either: record_sink is keyed on
       (class, source) and returns on the second arrival, so the finding, its reward and its envelope are all
       written once. What this oracle adds over the host's evaluation is the RUNG — `escaped` is raised here,
       at the one place that knows the delivered address survived as a `javascript:` one, and a search whose
       candidate arrived and did not fire needs that number whether or not any navigation followed.
   THE TRUSTED TYPES COLUMN IS THE SPEC'S, NOT THIS ENGINE'S CODE PATH: TT §3.8 makes the markup sinks
   TrustedHTML sinks and `eval` a TrustedScript sink, and navigating to a `javascript:` URL is not a TT sink at
   all — which is why the URL row declares none, and why its absence from the record is a positive statement
   rather than a gap.
   The cold tier's parked-candidate DCHECK names this table as where a parked candidate's sink re-binds BY NAME
   on resume; `sink_class_of_name` is that binding. */
struct SinkClass {
    const char       *name;      /* the display name a report and a parked entry carry */
    const char      **vectors;   /* the FIXED breakouts of a SINGLE-CONTEXT sink, NULL where `derive` builds them */
    int               derive;    /* SINK_DERIVE_* — which parser reads this sink's OWN output for its context */
    /* The `CspInlineType` (CSP §4.2.3's `type`) a fired breakout turns on, or -1 — which is not "unknown" but
       the positive statement that this sink is governed by no INLINE check at all: `eval` is §4.4.1's question
       about string compilation, which has no element, no type and no §6.8.2 mapping. Same shape and same
       reason as the `tt` column below. */
    int               policy;
    int               tt;        /* the TrustedTypeKind gating this sink, or -1 — the spec makes it no TT sink */
    /* WHETHER A FIRED BREAKOUT BECOMES A QUEUED PROGRAM OF THE FLOW, or is run by the sink itself. It is a
       fact about the CLASS and it decides whether `fires` is a number this search can have at all: an eval
       sink EVALUATES its own argument (§19.2.1 / §20.2.1.1.1), so there is nothing to queue and nothing to
       count; a markup breakout becomes a program only if the real parse put its marker in an auto-firing
       handler, and a URL one only if the delivered address is still a `javascript:` URL. Declared here rather
       than asked with a strcmp over `fires_on`, which is the display-text chain this table already replaced
       once. */
    int               queues_fire;
    const char       *fires_on;  /* what makes the fired breakout RUN, from the oracle above */
};
static const SinkClass SINKS[] = {
    [SINK_EVAL] = { "eval",      NULL,      SINK_DERIVE_JS,   -1,                          TRUSTED_TYPE_SCRIPT, 0, "sink-evaluates" },
    [SINK_HTML] = { "innerHTML", NULL,      SINK_DERIVE_HTML, CSP_INLINE_SCRIPT_ATTRIBUTE, TRUSTED_TYPE_HTML,   1, "parse-insert"   },
    /* §6.8.2 maps the inline type "navigation" to `script-src-elem`, NOT to `script-src-attr` — so
       `script-src 'unsafe-inline'; script-src-attr 'none'` must NOT block a javascript: URL. */
    [SINK_URL]  = { "location",  CANDS_URL, SINK_DERIVE_NONE, CSP_INLINE_NAVIGATION,       -1,                  1, "navigation"     },
};
#define SINK_CLASS_N ((int)(sizeof SINKS / sizeof SINKS[0]))

/* WHAT A DERIVED-CONTEXT SINK'S SEARCH OPENS WITH: an inert LOCATOR injected at the source in place of a
   breakout, so that ONE re-run of the real page shows the derivation where the attacker's bytes actually land
   — after the page's own filters, concatenations and re-encodings, which is the observation §@S(2) requires
   and which no static shape of the expression can make. It is a candidate flow like every other: it re-runs
   the page, so it costs what a breakout costs and it counts as one in `tried`. */
/* THE DELIVERY PROBE'S TOKEN — the second inert locator, and it measures a different thing from the first.
   The CONTEXT probe (below) answers "which §13.2.5 / §12 state are the attacker's bytes in"; this one answers
   §@S(2)'s other half, "which BYTES arrive at all", and the two cannot be one probe: the context locator is
   ASCII alphanumeric precisely so it cannot change the parse it measures, and a probe carrying `<` would
   change that parse into a different document.
   IT IS THIS FILE'S AND NOT A CLASS'S, because byte provenance has no language in it — solve_filter.c makes
   the same argument about the rung it owns. So one token serves every sink class and the observation is taken
   at the class-independent point (filter_survived), where a candidate's bytes are measured wherever they
   surface rather than only at the sink of their own class. */
#define SOLVE_BYTES_LOCATOR "apiclientbytes"

/* ONE TOKEN PER BYTE, WITH THE BYTE BEHIND IT, so the answer is per-byte and exact rather than an alignment
   guess over a mangled string: the character immediately after `apiclientbytesK` in the sink's output either
   IS the K'th byte of the set or it is not, and every way of not being it — percent-encoded (`%3C`), dropped,
   entity-escaped, moved — is the same answer for a fitness (solve_filter.h states why).
   THE SET IS THE SOURCE'S OWN DECLARATION, asked of the ONE registry (concolic.c) rather than copied: what the
   browser percent-encodes on the way in is exactly the list of bytes whose arrival is in question, and a byte
   no component transforms needs no probe to say it survives. Caller frees. */
static char *bytes_probe(const char *encodes) {
    size_t tl = sizeof SOLVE_BYTES_LOCATOR - 1, n, i, o = 0;
    char *s;

    DCHECK(encodes != NULL && *encodes,
           "the @S byte-delivery probe was built for a source that declares no percent-encode set — there is "
           "then no byte whose arrival is in question, and the probe would be a document re-run measuring "
           "nothing");
    n = strlen(encodes);
    DCHECK(n <= 10,
           "a source declared more percent-encoded bytes than the delivery probe can index — each byte is "
           "addressed by ONE decimal digit appended to the token, which is what makes the observation exact "
           "rather than an alignment guess, so an eleventh byte would be read back as the first");
    s = malloc(n * (tl + 2) + 1);
    CHECK(s != NULL, "solve: OOM building the @S byte-delivery probe");
    for (i = 0; i < n; i++) {
        memcpy(s + o, SOLVE_BYTES_LOCATOR, tl); o += tl;
        s[o++] = (char)('0' + (int)i);
        s[o++] = encodes[i];
    }
    s[o] = 0;
    return s;
}

static const char *derive_probe(int derive) {
    switch (derive) {
    case SINK_DERIVE_HTML: return SOLVE_HTML_LOCATOR;
    case SINK_DERIVE_JS:   return SOLVE_JS_LOCATOR;
    default: break;
    }
    DFAIL("a sink class declared a context derivation this file has no probe for - the probe is the run the "
          "derivation reads its context from, so a derivation without one seeds no candidates at all and the "
          "sink reports as parked forever");
    return NULL;
}

/* CHECK rather than DCHECK: every row of the report is written straight out of this table, so an index it does
   not have is the report reading past its own data. */
static const SinkClass *sink_class(int sink) {
    CHECK(sink >= 0 && sink < SINK_CLASS_N,
          "an @S record named a sink class this table does not have — the whole report is written from it");
    return &SINKS[sink];
}
static const char *sink_name(int sink) { return sink_class(sink)->name; }

/* THE NAME A CANDIDATE FLOW CARRIES, BACK TO ITS CLASS. A flow holds `cand_sink` as the table's OWN pointer,
   so the binding is identity and not a string compare — and a name from anywhere else is a candidate this
   table did not seed, which is precisely what a cold resume that rebinds by text must not produce. */
static int sink_class_of_name(const char *name) {
    int i;
    for (i = 0; i < SINK_CLASS_N; i++) if (SINKS[i].name == name) return i;
    DFAIL("a candidate flow carried a sink name that is not one of the sink classes' own — the name is this "
          "table's pointer, so a flow holding another was built somewhere that does not go through it (a cold "
          "resume rebinding a parked candidate BY NAME must land back on this table's row)");
    return -1;
}

/* Installed AFTER the table so the one point every session goes through can assert it — see the loop. */
void solve_init(JSContext *ctx) {
    g_pending = NULL; g_pending_n = g_pending_cap = 0;
    g_cands_seeded = 0;
    g_sink_reached = g_sink_tainted = g_sink_suppressed = 0;
    g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
    /* EVERY SINK CLASS DECLARES ITS WHOLE ROW. A row IS the sink's contract — where its breakout comes from,
       the CSP question, the Trusted Types question, and the fire semantics a PoC states — and a row added with
       a field left out does not fail: it emits a finding missing exactly that fact, which is the silent
       half-envelope this record exists to end.
       WHERE THE BREAKOUT COMES FROM IS ONE ANSWER, NOT TWO, so the assert is an EXCLUSIVE OR and not two
       presence checks: a class carrying both a written-down vector set and a context derivation would run the
       derivation and then also spray the list it was built to delete, and a class carrying neither is seeded
       nothing at all and reports as parked forever with no search having run.
       (`tt` is deliberately not asserted non-negative: -1 is the POSITIVE statement that the standard makes
       this sink no Trusted Types sink at all, which is what the URL row means.) */
    for (int i = 0; i < SINK_CLASS_N; i++) {
        DCHECK(!SINKS[i].queues_fire == !!(SINKS[i].derive == SINK_DERIVE_JS),
               "a sink class disagrees with itself about who runs a fired breakout — the JS-context class is "
               "the one whose sink EVALUATES its own argument, so it is exactly the class that queues no "
               "program, and any other pairing means one of the two was changed without the other");
        DCHECK(SINKS[i].name && SINKS[i].fires_on,
               "a sink class was declared without its display name or the fire semantics its oracle gives it — "
               "a PoC cannot state how it reproduces without that row being whole");
        DCHECK(!!(SINKS[i].vectors && SINKS[i].vectors[0]) != (SINKS[i].derive != SINK_DERIVE_NONE),
               "a sink class declared both a fixed vector set and a context derivation, or neither — a "
               "breakout comes from exactly one of the two, and a class with neither is seeded no candidates");
    }
    /* THE TWO DERIVED CLASSES' LOCATORS ARE THE PARTITION BETWEEN THEIR PROBES, so neither may contain the
       other. A page that writes one attacker source into BOTH an eval sink and a markup sink runs each class's
       probe straight past the other class's sink, and a locator one substring-test could confuse would make
       that sink derive a context for a search that never asked for it. candidate_search declines the write on
       the CLASS, so the two would then disagree about which search owns it. */
    DCHECK(!strstr(SOLVE_JS_LOCATOR, SOLVE_HTML_LOCATOR) && !strstr(SOLVE_HTML_LOCATOR, SOLVE_JS_LOCATOR),
           "the @S markup and JS context locators are not distinct — one contains the other, so the substring "
           "test that routes a probe's output to its own derivation answers for both");
    /* AND THE DELIVERY PROBE IS A THIRD, measuring a different question at the same sinks. Its token is what
       tells a byte-provenance run apart from a context run and from a breakout, so a token either of the
       other two contains would make one class's probe derive a context out of a string built to carry `<`. */
    DCHECK(!strstr(SOLVE_BYTES_LOCATOR, SOLVE_HTML_LOCATOR) && !strstr(SOLVE_HTML_LOCATOR, SOLVE_BYTES_LOCATOR) &&
           !strstr(SOLVE_BYTES_LOCATOR, SOLVE_JS_LOCATOR)   && !strstr(SOLVE_JS_LOCATOR, SOLVE_BYTES_LOCATOR),
           "the @S byte-delivery locator is not distinct from a context locator — one contains the other, so a "
           "probe built to carry the bytes a source encodes would be routed into a context derivation and the "
           "state it reported would be a state of the probe rather than of the page");
    DCHECK(!strstr(SOLVE_BYTES_LOCATOR, "X9"),
           "the @S byte-delivery locator carries the fire marker's own bytes — the probe is inert by "
           "construction and a marker in it would be recorded as a breakout arriving at its sink");
    /* THE JS-CONTEXT SINK'S OWN SEAM. The other two classes are reached from the browser component that
       performs them — a markup sink from the innerHTML setter, a URL sink from the navigation — because the
       host owns those operations. `eval` and the Function constructor are ECMAScript intrinsics that this host
       does not own, so the ENGINE announces them (JSEvalSinkFunc) and the detector is registered here, in the
       same call that installs the marker they fire. Without it this file's whole JS-context half was reachable
       only from a fixture that had overridden the global `eval` with a stand-in, and a real page's
       `eval(prefix + attackerInput)` was detected by nothing at all. */
    JS_SetEvalSinkHook(solve_eval_sink);
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "X9", JS_NewCFunction(ctx, js_x9, "X9", 0));
    JS_FreeValue(ctx, g);
}

/* THE ONE PLACE A SINK BECOMES PENDING — find-or-create, answering WHICH of the two it did. The distinction is
   load-bearing and used to be spelled as a `tried` test at the seeder: only a NEWLY detected sink opens a
   search (its class's written-down vectors, or its context probe), because a resumed parked candidate is
   already ONE of that search's flows and re-opening it would seed the whole search a second time.
   NAMED FOR WHAT IT ANSWERS AND NOT FOR THE ARRAY IT LIVES IN, because `pending_entry` is already a name:
   pending.h's reply register owns it, and flow.h puts that declaration in front of this file. The compiler
   caught the duplicate, but the collision was the smaller half of the problem — "pending" means the replies a
   flow is waiting on in one component and a detected-but-unsolved sink in this one, and one word carrying two
   meanings across two vocabularies is how a set of names goes wrong. What this returns is one sink's SEARCH. */
/* THE SEARCH FOR A (source, class) IF THERE IS ONE — the READ half, separate from sink_search because
   find-or-CREATE is the wrong primitive for a caller that is merely observing: a lookup that created one would
   fabricate a search with no breakouts, no path and nothing to seed, and every later reader of g_pending would
   take it for a detected sink.
   THE ARGUMENT THAT USED TO STAND HERE WAS ONE ABOUT PAYMENT — "opening a search is an EVENT (it credits the
   running flow)" — AND THAT IS NO LONGER A PROPERTY OF THIS CALL. The credit moved to where the OBSERVATION
   is, which is detection opening the search (add_pending), because creating the array slot and opening the
   search are two facts and the cold tier makes them come apart. Creating one now costs nothing and is still
   wrong, for the reason above; a reader who re-derives the payment argument will find it at the open. */
static Cand *search_of(const char *src, int sink) {
    for (int i = 0; i < g_pending_n; i++)
        if (g_pending[i].sink == sink && !strcmp(g_pending[i].src, src)) return &g_pending[i];
    return NULL;
}

static Cand *sink_search(const char *src, int sink, int *created) {
    Cand *e;

    DCHECK(src && created, "a sink was registered as pending with no source, or with nowhere to say whether "
                           "this call is the one that opened its search");
    *created = 0;
    for (int i = 0; i < g_pending_n; i++)
        if (g_pending[i].sink == sink && !strcmp(g_pending[i].src, src)) return &g_pending[i];
    if (g_pending_n >= g_pending_cap) { g_pending_cap = g_pending_cap ? g_pending_cap * 2 : 8; g_pending = realloc(g_pending, (size_t)g_pending_cap * sizeof(Cand)); CHECK(g_pending, "solve: OOM pending"); }
    /* EVERY field, because the array is realloc'd and never zeroed: leaving `tried` as whatever the allocator
       held made a sink look already-searched and it got no candidate at all. */
    e = &g_pending[g_pending_n++];
    e->src = strdup(src);
    CHECK(e->src, "solve: OOM pending");
    /* NOT LEARNED HERE, because this is find-or-create over (source, class) and its three callers reach the
       root by three different routes. Detection reads it off the value that arrived (add_pending); a parked
       candidate coming back from the cold tier takes it out of the record it was rebuilt from
       (solve_resume_candidate); the third — a candidate arriving at its own sink — has no route to one at all
       and asserts that it CREATED nothing, so the NULL this line writes is one no reader can reach. Both
       CREATING callers state the root before the entry is visible to anybody, which is exactly what
       emit_delivery's assert holds them to. */
    e->root = NULL;
    e->sink = sink;
    e->tried = 0;
    e->reached = 0;
    /* SAME LINE AND SAME REASON AS THE FIELDS AROUND IT: the array is realloc'd and never zeroed, and a latch
       left holding whatever the allocator had would silently spend a rung this search has never been paid. */
    e->reach_credited = 0;
    e->turns = 0;
    e->fires = 0;
    /* THE TWO OBSERVATION COUNTS TAKE THE SAME LINE AS EVERY FIELD AROUND THEM, and the reason is sharper for
       these two than for a ratchet: the array is realloc'd and never zeroed, and BOTH of these are read as
       "was this observation ever made". A garbage nonzero does not merely misreport a quantity — it states
       that a substitution happened, or that a sink ran, for a search that has never once been given the
       thread, which is the confident-wrong-instruction direction this pair exists to remove. */
    e->substituted = 0;
    e->sink_strings = 0;
    /* AND THE RUNWAY RATCHET TAKES THE SAME LINE FOR THE REASON THE SENTENCE ABOUT `surv_len` GIVES, which is
       the sharper one of the two: a best-so-far left holding garbage does not merely misreport — it makes
       every real observation compare against a maximum nothing ever reached, so the field never moves again
       and reads as a candidate that consumed none of its path for the rest of the session. */
    e->replay_pm = 0;
    /* THE TWO NEW RUNGS TAKE THE SAME LINE AS THE OTHER FOUR, and the comment above this block is why: the
       array is realloc'd and never zeroed, so a field left out here reads whatever the allocator held. For a
       best-so-far ratchet that is not merely a wrong report — a garbage `surv_len` makes the first real
       observation compare against a maximum nothing ever achieved and the rung never pays at all. */
    e->surv_run = 0; e->surv_len = 0;
    /* THE OFFSETS OF A RUN THAT DOES NOT EXIST YET — -1 and not 0, for the reason FilterObs gives about its
       own pair: 0 is a real offset, so a zeroed pair states that a run this search has never observed begins
       at the candidate's first byte. Same realloc'd-and-never-zeroed line as every field around it. */
    e->surv_at = -1; e->surv_out = -1;
    /* EVERYTHING DELIVERS UNTIL SOMETHING CONTRADICTS IT — the sound-only direction (solve_filter.h): a search
       that has been told nothing keeps every arm, exactly as a branch whose domain permits both outcomes keeps
       both. The array is realloc'd and never zeroed, so an omission here would read a constraint out of
       whatever the allocator held and decline escapes at random.
       THE PERMISSIVE FILL IS ALL THIS LINE CAN HONESTLY DO, and the narrowing is not deferred to a run alone:
       the root's carrier refuses some bytes outright and that is knowable without any run, but the root is not
       known HERE — this is find-or-create over (source, class), whose callers reach the root by routes of their
       own, which is exactly what the `e->root = NULL` above says. cand_learn_root is where that fact arrives,
       so cand_learn_root is where the declaration's half of this table is seeded. */
    solve_delivered_all(&e->deliv);
    e->deliv_seen = 0;
    e->deliv_runs = 0;
    e->wit = NULL; e->nwit = e->witcap = 0;
    e->reinject = NULL;
    /* AND ITS LENGTH ON THE SAME LINE AS THE POINTER, for the reason the ratchets above take: the array is
       realloc'd and never zeroed, and this field is read as "did this search have a recorded path at all" —
       so a garbage nonzero does not misreport a size, it states that arms were offered to candidates that
       were offered none, which is the confident-wrong-instruction direction the pair exists to remove. */
    e->reinject_len = 0;
    /* AND THE PAIR ON THE SAME LINE AS THE RATCHET IT REPLACES THE READING OF, for that ratchet's own reason:
       the array is realloc'd and never zeroed, and a best-so-far left holding garbage compares every real
       observation against a maximum nothing reached, so the pair would never move again and would report a
       replay that walked arms as one that walked none. `of` is additionally the "never observed" flag, so a
       garbage nonzero there states that a reading was taken for a search that has never run. */
    e->replay_arms = 0; e->replay_of = 0;
    /* SAME LINE AND SAME REASON AS `reach_credited`: the array is realloc'd and never zeroed, and a latch left
       holding whatever the allocator had spends a rung this search has never been paid. */
    e->escaped = 0; e->escape_credited = 0;
    e->pl = NULL; e->npl = e->plcap = 0; e->seeded = 0;
    /* NOTHING HAS COME BACK FROM THE COLD TIER YET, on the same line as every field around it: the array is
       realloc'd and never zeroed, and a garbage nonzero here does not merely misreport a quantity — it excuses
       exactly the arrival the assert beside it exists to catch, which is the confident-wrong direction. */
    e->resumed = 0;
    /* SAME LINE AND SAME REASON AS THE FIELD ABOVE IT, and sharper: the array is realloc'd and never zeroed,
       and add_pending reads this number as the positive statement that a withdrawal accounts for an entry
       nothing has tried. A garbage nonzero here would EXCUSE the third-door arrival that assert exists to
       catch, which is the confident-wrong direction rather than a misreported quantity. */
    e->resumed_withdrawn = 0;
    /* THE SEARCH IS NOT OPEN YET, AND THE SLOT EXISTING IS NOT THE SAME FACT — see add_pending. The array is
       realloc'd and never zeroed, so a latch left holding whatever the allocator had would make a search read
       as already opened and it would never get its probe. */
    e->opened = 0;
    *created = 1;
    return e;
}

/* ADD A BREAKOUT TO THIS SINK'S SEARCH, deduped by its TEXT. A probe run reaches one sink as often as the page
   writes it (a loop over innerHTML), and two occurrences of the source can land in the same tokenizer state,
   so the same constructed escape arrives more than once — and each duplicate would otherwise be a whole extra
   re-run of the page that can only reproduce a result already had. */
static void push_breakout(Cand *e, const char *payload, int kind) {
    DCHECK(e && payload && *payload, "a breakout was queued onto no sink, or with no bytes in it");
    /* THE KIND IS STATED BY THE PUSHER AND ASSERTED HERE, because it is the one fact about these bytes that
       cannot be recovered from them afterwards. Every reader below acts on it — the seeder declines to
       withdraw an instrument, the arrival assert asks whether the bytes could have been BUILT by this search,
       the report splits `probes` from attacks — so a third value, or a zero left by a caller that did not
       think about it, would put an entry in a state every one of those readers answers differently for. */
    DCHECK(kind == CAND_PROBE || kind == CAND_ESCAPE,
           "a candidate was queued as neither an instrument nor an attack — a probe is inert and is never "
           "withdrawn and never counted as an arrival, an escape is the opposite on all three, and there is no "
           "third thing for this list to hold");
    /* DEDUPED BY TEXT, AND THE KIND IS PART OF WHAT THAT SETTLES. Two producers can reach the same bytes — a
       written-down vector and a derivation, or a derivation re-run under a narrowed table — and a duplicate
       would otherwise be a whole extra re-run of the page that can only reproduce a result already had. An
       entry keeps the kind it was FIRST pushed with, which is sound because the two producers that can collide
       are both escape producers: a probe is built out of this file's own locators (derive_probe, bytes_probe)
       and no derivation emits one, so a collision between an instrument and an attack is not reachable. It is
       asserted rather than assumed, because that argument is about the locator vocabulary and a class added
       later owns its own. */
    for (int i = 0; i < e->npl; i++)
        if (!strcmp(e->pl[i].bytes, payload)) {
            DCHECK(e->pl[i].kind == kind,
                   "one candidate's bytes have been queued as an instrument by one producer and as an attack "
                   "by another — a probe carries this file's own locator and a derivation constructs from the "
                   "sink's grammar, so identical bytes from both means a class's probe vocabulary has come to "
                   "overlap its escapes and every reader of the kind now answers for whichever pushed first");
            return;
        }
    if (e->npl >= e->plcap) {
        e->plcap = e->plcap ? e->plcap * 2 : 8;
        e->pl = realloc(e->pl, (size_t)e->plcap * sizeof(CandPayload));
        CHECK(e->pl, "solve: OOM recording a breakout for a sink search");
    }
    e->pl[e->npl].bytes = strdup(payload);
    CHECK(e->pl[e->npl].bytes, "solve: OOM recording a breakout for a sink search");
    e->pl[e->npl].kind = kind;
    e->pl[e->npl].surv = 0;
    e->npl++;
}

/* HOW MANY OF THIS SEARCH'S CANDIDATES ARE INSTRUMENTS — the report's `probes`, READ OFF THE LABELS rather
   than off a leading count, so it states what the entries ARE and not where they happen to sit. */
static int cand_probes(const Cand *e) {
    int n = 0;
    DCHECK(e != NULL, "the probe count was asked of no search");
    for (int i = 0; i < e->npl; i++) if (e->pl[i].kind == CAND_PROBE) n++;
    return n;
}

/* …AND WHETHER ONE OF THEM IS THE DELIVERY PROBE — read off the entries this search HOLDS rather than by
   re-deciding add_pending's rule for pushing one. That rule has two terms (the class derives, and the source
   declares a percent-encode set) and a second copy of it here would be the third statement of one fact, of
   which the copy nobody runs against reality is the one that drifts. The partition is the probe's own locator,
   which is the SAME one observe_delivery routes the observation on, so there is one rule and this reads it.
   IT IS WHAT MAKES `deliveryProbed` ABSENT RATHER THAN ZERO for a search that has no such probe — a
   single-context class states its vectors at detection and runs none, and a derived search over server-injected
   page state has no byte whose arrival is in question. A 0 for either would say "the probe never arrived" about
   a search that has no probe, which is the reading `witnessed` and `fires` are absent for. */
static int cand_has_delivery_probe(const Cand *e) {
    DCHECK(e != NULL, "the delivery-probe question was asked of no search");
    for (int i = 0; i < e->npl; i++)
        if (e->pl[i].kind == CAND_PROBE && e->pl[i].bytes &&
            !strncmp(e->pl[i].bytes, SOLVE_BYTES_LOCATOR, sizeof SOLVE_BYTES_LOCATOR - 1)) return 1;
    return 0;
}

/* HAS THIS SEARCH CONSTRUCTED AN ESCAPE? — the question `npl > nprobe` was the arithmetic for. It is a
   statement about what the list HOLDS, so it is answered by asking the entries, and it is then true in every
   order the two producers can arrive in rather than only in the one the leading count was taken in. */
static int cand_has_escape(const Cand *e) {
    DCHECK(e != NULL, "the escape question was asked of no search");
    for (int i = 0; i < e->npl; i++) if (e->pl[i].kind == CAND_ESCAPE) return 1;
    return 0;
}

/* WHAT KIND OF THING THESE EXACT BYTES ARE TO THIS SEARCH, or 0 for bytes this session's record does not hold.
   THE ZERO IS A POSITIVE STATEMENT AND ITS ONE LEGITIMATE PRODUCER IS THE COLD TIER: a resumed candidate's
   payload rides the resumed FLOW rather than this session's record (solve.h, on `payloads` being empty beside
   a non-zero `tried`), so it has no row here unless this session's own derivation independently constructed
   the same bytes — in which case it is the same payload, one row, which is what deduping by text means.
   Every caller of this therefore reads the zero against `Flow.cand_resumed` and never as "not found". */
static int cand_kind_of(const Cand *e, const char *bytes) {
    DCHECK(e != NULL && bytes != NULL, "a candidate's kind was asked of no search, or about no bytes");
    for (int i = 0; i < e->npl; i++) if (!strcmp(e->pl[i].bytes, bytes)) return e->pl[i].kind;
    return 0;
}

/* CAN THIS SEARCH ACCOUNT FOR THE BYTES THAT JUST ARRIVED AT ITS SINK? — breakout_arrived's whole condition,
   held as ONE call so it can be spelled inside the DCHECK. A release build type-checks a DCHECK's condition
   and never evaluates it, so written as a local read before the assert this scan would run on the shipped
   arrival path for a check that build does not make; written as the condition it costs exactly nothing there.
   It is side-effect-free, which is what a DCHECK condition must be (check.h). */
static int cand_arrival_is_attack(const Cand *e, const Flow *f) {
    int kind;
    DCHECK(e != NULL && f != NULL && f->cand_payload != NULL,
           "the arrival question was asked of no search, or of a flow carrying no payload — the caller has "
           "already CHECKed both, so reaching here without them means a second route into this rung");
    kind = cand_kind_of(e, f->cand_payload);
    return kind == CAND_ESCAPE || (kind == 0 && f->cand_resumed);
}

/* THE FIRST OF A SOURCE'S DECLARED BYTES THAT ITS OWN DELIVERY TABLE REFUSES, or 0 — the whole of
   cand_learn_root's two-sided check, held as ONE call so it can be spelled inside the DCHECKF rather than run
   as a loop the shipped build would walk for an assert it does not make (the same shape and the same reason as
   cand_arrival_is_attack). Side-effect-free, which is what a DCHECK condition must be (check.h). 0 is not a
   byte a component can declare: bytes_probe indexes the set by one decimal digit appended to its token, so a
   NUL in it would end the probe string, and every declared set in the tree is printable by construction. */
static int declared_byte_refused(const SolveDelivered *d, const char *enc) {
    int i;
    for (i = 0; enc && enc[i]; i++)
        if (!d->ok[(unsigned char)enc[i]]) return (unsigned char)enc[i];
    return 0;
}

/* THE SEARCH LEARNS HOW THE ATTACKER'S BYTES ARRIVE — once, from the value that arrived. A source reaching a
   sink twice reaches it by the same route both times: the root is inherited unchanged through every derivation,
   so two values with the same injection identity cannot have entered by two. Asserted rather than overwritten,
   because if it ever were two the report would state whichever detection ran last. */
static void cand_learn_root(Cand *e, const char *root) {
    DCHECK(e && root, "a sink search was told how its bytes arrive by nothing, or was told nothing");
    if (!e->root) { e->root = strdup(root); CHECK(e->root, "solve: OOM recording a sink's delivery root"); }
    else DCHECK(!strcmp(e->root, root),
           "one sink search has been handed two different delivery ROOTS for one injection identity — the root "
           "is inherited unchanged through every derivation, so two values spelling the same source cannot have "
           "entered the program by two different components, and the envelope would report whichever detection "
           "ran last");
    /* …AND THE DELIVERY TABLE TAKES THE HALF OF ITS ANSWER THAT NO RUN CAN GIVE IT, HERE, because this is the
       one moment the root becomes known and BOTH of the search's doors pass through it — detection
       (add_pending) and the cold tier's rebuild (solve_resume_candidate). The `return` that stood on the
       first branch is gone for exactly that reason: on a found entry this call is the equality assert, and it
       must still be the seed, or the search whose slot a resume created would be seeded by whichever door
       happened to run second.
       WHAT IT SEEDS IS NOT THE DECLARED ENCODE SET, and the distinction is concolic.c's to make rather than
       this file's — which is why this is a call and not a table. §@S(2) is measured, so what the browser
       percent-encodes on the way IN is a PRIOR (a page that decodes its own fragment receives the byte, and
       this engine already fires a markup PoC through that round trip) and only a run settles it. What a
       CONSTRAINED carrier refuses is not a prior: the byte never enters the page's program in any form, so no
       page-side transform recovers it and no run can widen it. The registry owns the column both halves are
       read from, so the registry answers, and this line asks.
       ORDER MATTERS AND IS WHY IT IS AT THE TOP OF ITS CALLERS: add_pending pushes a class's written-down
       vectors and its probes AFTER this, and solve_seed_candidates withdraws an escape the table refuses — so
       the table is complete before anything is queued against it rather than being narrowed underneath a list
       that was built while everything still delivered.
       IT IS NOT A MEASUREMENT AND DOES NOT PRETEND TO BE ONE: `deliv_seen` stays where it was, so
       `sourceDelivers` still reports only what a probe RAN and observed, and a declaration-narrowed table is
       not emitted as a measured set. */
    /* THE TWO-SIDED HALF, AND IT IS NOT A RESTATEMENT OF THE CALL — it reads the OTHER column of the same row.
       A carrier that refuses a byte its own row also DECLARES would be a declaration contradicting itself
       (root_carrier: the row lists the PRINTABLE bytes the production excludes, and the refusal covers
       everything outside printable US-ASCII, so the two cannot name one byte), and the cost of that landing
       quietly is the instrument itself: the delivery probe is BUILT out of the declared bytes (bytes_probe),
       so a seed that cleared one would have solve_seed_candidates withdraw the probe that was going to measure
       it — a search that can never learn the thing its own report is about. Asked only where the seed FIRED,
       so it is a question about a state that exists rather than one that cannot arise. */
    if (concolic_source_carrier_bytes(e->root, &e->deliv))
        DCHECKF(declared_byte_refused(&e->deliv, concolic_source_encodes(e->root)) == 0,
                "an @S source's carrier refuses byte 0x%02X while the same row DECLARES it as one the "
                "component percent-encodes — the declared set is the PRINTABLE bytes the carrier's own "
                "production excludes and the refusal covers everything outside printable US-ASCII, so the two "
                "cannot name one byte. The delivery probe is built out of the declared set, so this seed has "
                "just withdrawn the instrument that was going to measure it",
                (unsigned)declared_byte_refused(&e->deliv, concolic_source_encodes(e->root)));
}

/* A DETECTED SINK OPENS ITS SEARCH. A single-context class states its breakouts; every other class states the
   probe whose run the derivation reads its context from. */
static void add_pending(const char *src, const char *root, int sink) {
    int created = 0;
    Cand *e = sink_search(src, sink, &created);
    const SinkClass *sc;

    /* BEFORE THE EARLY RETURN, because most detections FIND this entry rather than create it: a source reaches
       one sink as often as the page writes it, and a cold-resumed candidate opens the search before any
       exploration flow of this session has re-reached it. On a found entry this call is the EQUALITY assert,
       which is the whole of what says the two ends of the tier agree about how these bytes arrive. */
    cand_learn_root(e, root);
    /* OPENING A SEARCH IS A PROPERTY OF THE SEARCH, NOT OF WHO MADE THE SLOT, and this line used to be
       `if (!created) return;` — the same defect as the leading probe count, one level up and with worse
       consequences. Two independent producers reach g_pending: DETECTION, which is the only one that can open
       a search (it is the one moment a flow stands at the sink holding the value that arrived, which is what
       the probe and the re-injection point are both taken from), and a COLD RESUME, which registers a
       candidate of a search opened in an earlier session. `created` conflates "this call made the array slot"
       with "this call opened the search", and those come apart in exactly one order — the one the cold tier
       makes STRUCTURAL, since cold_resume runs at engine init and detection cannot run before a flow does. In
       that order the resume created the slot, so detection returned HERE: no context probe, no written-down
       vectors, no re-injection point, `search_seeds` false for ever. The session's search for that sink could
       then construct nothing, derive nothing and seed nothing, and the report said `payloads:[]` — which
       solve.h defines as the positive statement that this source can carry no exit from the state its bytes
       landed in. A silent wrong verdict produced by arrival order.
       ASKED OF THE ENTRY, it is right in both orders: whichever door made the slot, the first DETECTION opens
       the search, and the second and hundredth return here as they always did. */
    if (e->opened) return;
    /* …AND THE ONLY OTHER DOOR THAT CAN HAVE MADE THE SLOT SAYS SO IN ITS OWN NUMBERS. A detection opening a
       search on an entry it did not create means some other producer made that entry, and there is exactly one
       — solve_resume_candidate.
       IT ACCOUNTS FOR ITS RECORDS IN TWO NUMBERS AND NOT ONE, AND THIS ASSERT USED TO NAME ONLY THE FIRST. It
       read `created || e->tried > 0` under the argument that the rebuild "raises `tried` for every candidate
       it rebuilds", and that argument is retired: a record whose payload the root's carrier positively refuses
       is WITHDRAWN, which creates the slot — the delivery table has to exist before anything can be refused
       against it — and raises `resumed_withdrawn` instead. Under the old spelling the first such record made
       every later detection of that sink abort on an assert that was right about its own rule and wrong about
       the tree. Both terms are here because both are real evidence of the same producer, and neither is a
       widening: a third door (a future writer into g_pending that neither detects nor resumes nor withdraws)
       still shows up here rather than quietly acquiring a probe and a path it has no claim to. */
    DCHECK(created || e->tried > 0 || e->resumed_withdrawn > 0,
           "a sink search is being opened on an entry this detection did not create and no cold-resumed "
           "candidate accounts for — g_pending has two writers, detection and the cold tier's rebuild, and the "
           "second either RESUMES a record (raising `tried`) or WITHDRAWS one whose carrier refuses its "
           "payload (raising `resumed_withdrawn`), so an entry that predates this call with neither was put "
           "there by a third door and is about to be handed a context probe and a re-injection point on behalf "
           "of a search nobody opened");
    e->opened = 1;
    /* THE RE-INJECTION POINT IS A FACT ABOUT THE SEARCH, NOT ABOUT THE DERIVATION, so it is taken HERE — at the
       one moment a flow is standing at this sink holding the value that reached it. The field's own comment
       used to say this was the DERIVED classes' problem and that a single-context class "states its vectors at
       DETECTION, so its candidates are seeded when the frontier is small and one of them fires". The first half
       is true and the second does not follow, and the cost is measurable on the shipped artifact: a document
       with K independent concolic gates in front of one sink creates 2^K exploration flows, and EVERY candidate
       with no path re-searches that same tree for the one arm that arrives. Measured on extension/lib/qjs at
       e718ef9f, K=10: the eval and markup sinks each created 2049 flows = 1 + 2^10 explored + 2^10 forked by
       the CONTEXT PROBE, while the derived breakout — which already holds a path, frozen by queue_derived —
       forked NONE. At K=8 the URL sink created 768 = 2^8 + 2 x 2^8, because it has two written-down vectors and
       neither replayed. So the probe pays the traversal in full for every class, and pays it at the moment the
       search opens, which is the whole of what a `reached:0` beside a growing `turns` reports.
       IT IS THE SAME BLOB AND THE SAME SOUNDNESS ARGUMENT the field already states: a candidate run substitutes
       a CONCRETE value at the source, so branches on it are decided by running the real predicate and never
       from the vector — the recorded arms only replay the decisions the payload does not make. What differs is
       WHOSE path it is: the detecting flow's rather than the probe's. Both demonstrably reach this sink with
       this source, which is the only property a replayed path has to have.
       THIS IS THE ONLY CAPTURE. queue_derived's `if (!e->reinject) e->reinject = decide_freeze_path()` is gone
       with it, so there is one freeze, one owner and one release rather than two sites that had to agree about
       which path wins — and the field is now unambiguous enough to answer a second question (search_seeds). */
    DCHECK(flow_running() != NULL,
           "an attacker source reached a sink with no flow running — a concolic value is minted by a flow and "
           "carried by one, so there is no route to this line from outside the scheduler, and the path about to "
           "be frozen would be whatever chain the previously-switched-in flow left behind");
    /* AND THE FLOW THAT MADE THE OBSERVATION IS THE ONE PAID FOR IT. This credit — "a NEW
       attacker-source-reaches-sink: value-of-information for the running flow" — used to sit on the CREATE, in
       sink_search, which is the same conflation the `opened` latch above ends: a cold resume creating the slot
       is not an observation of anything (it re-registers a search a previous session opened), and cold_resume
       runs at engine init where there is no running flow at all — so in that order the credit was dropped on
       the floor and the exploration flow that later did the real detecting was paid nothing for it. The
       observation is the OPEN, so the credit is at the open. Below the assert above rather than beside the
       latch, so that "no flow is running here" aborts as the invariant it is instead of being spent as a
       silently-skipped payment. */
    flow_credit_emit(1.0);
    e->reinject = decide_freeze_path();
    /* AND HOW MANY ARMS THAT PATH HOLDS, TAKEN HERE BECAUSE THIS IS WHERE THE PATH EXISTS AND IS OWNED. It is
       a SIZE fixed from this instant (decide.h's `entries`), it is the whole of what `runwayArms` reports,
       and it is stored rather than re-derived because record_sink gives the blob back at the fire — a length
       read at the emitter would be 0 for exactly the searches that succeeded.
       IT PROMOTES NOTHING, WHICH IS A QUESTION THIS LINE HAD TO ANSWER RATHER THAN A REASSURANCE: the
       accessor's null guard is an always-fatal CHECK, so a NULL blob here would be a release-mode abort where
       the DCHECK twenty lines down is the only thing that looks today. decide_freeze_path cannot hand one
       over — it CHECKs its own allocation and returns that pointer unconditionally — so the argument is
       non-NULL by construction and this call adds no failure mode to a release build.
       AND A CHAIN OF ZERO LENGTH IS A LEGITIMATE ANSWER AND NOT ONE TO ASSERT AGAINST. A flow that has
       decided nothing freezes a blob whose segment is absent, which the accessor reports as 0 entries — that
       is exactly the `runwayArms:0` reading below, so a DCHECK demanding a nonzero here would abort on the
       one state this field was added to be able to state. */
    {
        long arms = 0;
        DCHECK(e->reinject_len == 0,
               "a search's recorded path length was written twice — the freeze above is the ONE capture and "
               "the `opened` latch is what makes it one, so a second write means this entry reached the "
               "capture again and the first value described a path this search no longer stands on");
        decide_blob_stats(e->reinject, &arms, NULL);
        DCHECK(arms >= 0 && arms <= 0x7fffffff,
               "a frozen decision path reports a length that is not a count of slots — `runwayArms` is read "
               "as whether this search offered its candidates any arm at all, so a negative value or one "
               "truncated by the store below would publish that answer on something that is not a length, "
               "and 0 is the reading the whole pair turns on");
        e->reinject_len = (int)arms;
    }
    sc = sink_class(sink);
    /* A SINGLE-CONTEXT CLASS'S WRITTEN-DOWN VECTORS ARE ATTACKS, which is what `probes:0` beside a non-empty
       list states and what used to be spelled by leaving `nprobe` at 0 for this arm. Said rather than
       implied: the seeder withdraws a contradicted one of these and the report marks it, both of which are
       correct for a vector and wrong for an instrument. */
    if (sc->vectors) { for (int c = 0; sc->vectors[c]; c++) push_breakout(e, sc->vectors[c], CAND_ESCAPE); }
    else {
        /* THE INSTRUMENTS, LABELLED AS INSTRUMENTS. They used to be told apart by being pushed FIRST and
           counted, which required this to be the one moment nothing else had been pushed; the label carries
           the fact instead, so it survives an entry this call did not create. */
        push_breakout(e, derive_probe(sc->derive), CAND_PROBE);
        /* AND THE DELIVERY PROBE BESIDE IT, for the class that DERIVES and only for it. That is routing and
           not an exception: the table it fills is read by a derivation choosing between two spellings of one
           exit transition, so a class whose breakouts are WRITTEN DOWN has nothing to read it, and a probe for
           one would be a document re-run whose answer no construction consults.
           IT IS SKIPPED WHERE THE SOURCE DECLARES NOTHING, which is a fact and not an omission: server-injected
           page state (`window.__FLAGS`) is written by the attacker directly, no component percent-encodes it,
           and there is no byte whose arrival is in question. */
        {
            const char *enc = concolic_source_encodes(root);
            if (enc && *enc) {
                char *bp = bytes_probe(enc);
                push_breakout(e, bp, CAND_PROBE);
                free(bp);
            }
        }
    }
    DCHECK(e->npl > 0 && e->reinject != NULL,
           "a search was opened with no candidate to run or with no path to run it on — the class states one of "
           "the two breakout sources (solve_init asserts the exclusive or) and this call is the one moment a "
           "flow stands at the sink, so either missing means the search would re-search the document's whole "
           "gate tree for an arm the detection already took");
}

static void record_sink(int cls, const char *source, const char *poc) {
    /* A finding is a pending sink that SOLVED, so the two lists are one list in two states — the parked-search
       emit subtracts one from the other by (sink, source) and a finding with no pending twin would report as
       both fired and parked. Asserted at the origin because a future detector that records a PoC without first
       calling add_pending would otherwise corrupt the report rather than crash.
       AND THE TWIN IS WHERE THE DELIVERY ROOT COMES FROM. A fire is observed at the marker, where the value
       that carried the attacker's bytes was concrete a long time ago — there is nothing left to read a root
       off — so the finding takes the one its own search learned at detection. It is the same fact, and it is
       held once. */
    /* BOTH ARE `CHECK`, BECAUSE BOTH ARE DEREFERENCED TWO LINES DOWN AND A DCHECK IS NOT THERE IN RELEASE.
       The `(twin && twin->root) ? … : NULL` these replaced was wrong for the reason the paragraph below gives,
       and deleting it was right — but it had been carrying the null guard, so in release the strdup below went
       straight through a NULL `twin`. A guard whose removal turns a dev-mode abort into a production segfault
       is a universal invariant (check.h: data integrity, must not PROCEED), not a should-never-happen; the two
       allocations on the next line already carry CHECK for the same reason. */
    Cand *twin = search_of(source, cls);
    CHECK(twin != NULL, "an @S finding was recorded for a sink that was never detected as pending");
    /* AND IT HAS ONE, WHICH IS A STATEMENT ABOUT THE SEARCH AND NOT A HOPE ABOUT THIS FINDING. Both doors into
       g_pending state the root — detection reads it off the value, a cold resume takes it out of the park
       record — so a NULL here names a third door rather than a search that happens not to know. It was a
       `(twin && twin->root) ? … : NULL` while the second door did not exist, and that ternary was the quiet
       half of the same defect: the loud half aborted in emit_delivery naming the missing field, while this one
       stored a FIRE-VERIFIED PoC with no envelope and said nothing at all. */
    CHECK(twin->root != NULL,
           "the search behind a fire-verified @S PoC never learned how its bytes arrive — the finding is the "
           "strongest thing this half of the tool emits, and without the root §S(d)'s reproduction envelope "
           "reports it as an exploit no navigation reaches");
    sink_class(cls);   /* the row exists before anything is stored against it */
    for (int i = 0; i < g_sinks_n; i++) if (g_sinks[i].cls == cls && !strcmp(g_sinks[i].source, source)) return;
    if (g_sinks_n >= g_sinks_cap) { g_sinks_cap = g_sinks_cap ? g_sinks_cap * 2 : 8; g_sinks = realloc(g_sinks, (size_t)g_sinks_cap * sizeof(Finding)); CHECK(g_sinks, "solve: OOM @S store"); }
    Finding *f = &g_sinks[g_sinks_n++];
    f->cls = cls; f->source = strdup(source ? source : "?"); f->poc = strdup(poc);
    f->root = strdup(twin->root);
    /* THE THREE ALLOCATIONS OF THE ONE RECORD THAT MUST SURVIVE, and none of them was checked before. A NULL
       here is not a lost finding, it is a CORRUPT one: `solved` strcmps the source to decide whether to also
       emit the sink as a parked search, and solve_json_array writes the poc straight into the report. Every
       neighbouring allocation — the store's own realloc one line up, the pending entry, each breakout — has
       carried a CHECK all along. The root's clause is now the same clause as the other two, because the root
       is now as unconditional as they are. */
    CHECK(f->source && f->poc && f->root,
          "solve: OOM storing a fire-verified @S PoC — the finding is the proof, and a half-stored one corrupts "
          "every later read of the report rather than losing it. The delivery ROOT is part of that proof: §S(d) "
          "requires every emitted PoC to carry its reproduction envelope, and a finding that lost its root "
          "reports as one no navigation reaches");
    /* AND THE FLOW THAT PROVED IT IS PAID FOR IT. sink_search already credits a sink merely being DETECTED,
       which is the weakest @S observation there is, while the strongest — a fire-verified PoC, the thing this
       half of the tool exists to produce — credited nothing at all. That is not only an inconsistency inside
       this file: a candidate flow records no endpoints by design (endpoint_suppress), so before this line its
       reward was zero for its whole life and it competed on the optimism term alone, which is capped at 1.0,
       against exploring flows whose reward is unbounded. The flow that just proved an exploit is the last one
       a WFQ should be aging out. */
    flow_credit_emit(1.0);
    /* AND THE SEARCH'S RE-INJECTION POINT IS GIVEN BACK, because this is the moment it stops being useful and
       starts being a ceiling. The blob pins the frozen decision segment the detection reached this sink on, and
       every segment below it, for as long as the search holds it; a solved search seeds no further candidates,
       so from here it is a prefix of the fork tree kept alive by nothing but a pointer nobody will read.
       CLEARING IT IS WHAT CLOSES THE SEARCH, not merely what keeps solve_free from releasing it twice. The
       sentence above used to be a claim about behaviour with nothing performing it — the probe's other arms go
       on arriving at this sink after a fire, and each breakout they derived was seeded for another full
       document re-run. search_seeds READS this NULL, so the release and the closure are one act at one site
       rather than a comment and a hope. */
    if (twin->reinject) { decide_blob_free(twin->reinject); twin->reinject = NULL; }
}

/* DETECTION — the tail all three sink classes share, because they ask the value the same two questions and
   this is the last point at which the value that carried the attacker's bytes still exists.
   WHICH SOURCE THIS IS is what an @S candidate is injected at, so the record is keyed by the value's own
   identity — a derived one included, because that is where a substitution has to land.
   HOW ITS BYTES ARRIVED is a different question and the envelope's, so it is asked with the value's ROOT.
   Every concolic has one (concolic_alloc asserts a provenance and a root are present together), so an absent
   one is a value minted somewhere that does not go through that mint, and what it costs is the envelope. */
static void detect_sink(JSValueConst arg, int cls) {
    const char *shape, *src, *root;

    /* THE THREE ARRIVAL FACTS, COUNTED AT THE ONE POINT ALL THREE SINK CLASSES CONVERGE ON — and the concolic
     * test is HERE for the same reason, rather than repeated in each caller.
     *
     * WHAT AN EMPTY @S SURFACE MEANS IS FOUR DIFFERENT THINGS AND IT REPORTED ONE NUMBER FOR ALL OF THEM. A
     * page with no entries is one of: no attacker source was ever read (counted upstream, in the component
     * that mints one — solver/concolic.h); no sink RAN, so nothing could arrive; a sink ran and what reached
     * it was the page's own strings, never anything tainted; or something tainted DID arrive and the search
     * was suppressed because the check standing on it was unforgeable. The first two are a driving gap, the
     * third is a page that may simply have no such flow, and the fourth is a POSITIVE result about the page —
     * so they take opposite actions, and an empty array is the same evidence for each. That is the defect this
     * file already ends twice over for `tried`/`reached` and for absence-versus-zero; it stood one layer up,
     * where it decides which surface is worth working on at all.
     *
     * THE COUNTS ARE EXPLORATION-ONLY BY CONSTRUCTION, not by a test written here: every caller returns inside
     * its own `is_verifying()` branch before reaching this line, so a candidate re-run's arrivals — which are
     * this engine's own injected bytes coming back — can never be counted as the page delivering taint to a
     * sink. That is the partition that would otherwise make `tainted` climb with the number of candidates.
     *
     * AND THE CONCOLIC TEST MOVED IN WITH THEM. It was three copies of `if (!concolic_is(arg)) return;`, one
     * per detector, which is the per-caller `if` on a question every caller asks identically — so a fourth
     * sink class would have had to remember it, and a class that forgot would call this function with a plain
     * string and abort on the root assert below rather than reporting an untainted arrival. One dispatch, N
     * callers.
     *
     * …AND THE ONE QUESTION THAT IS NOT EVERY CALLER'S IS ASSERTED HERE RATHER THAN ASKED. ECMAScript
     * §19.2.1.1 PerformEval ( source, strictCaller, direct ) step 2 — "If source is not a String, return
     * source" — makes a CONCRETE non-string offered to `eval` a call that compiles nothing, and `reached` is
     * defined one file up as how many times a code-execution sink was EXECUTED. The rule is the JS class's
     * alone: a URL sink and a markup sink both run ToString on whatever they are handed, so
     * `location = { toString(){ return "javascript:…" } }` is a real vector and declining an object there
     * would delete a true finding. So the class that owns the rule asks it, and this — the point every class
     * converges on — states it, which is what stops the two from drifting into disagreement silently. */
    DCHECK(cls != SINK_EVAL || JS_IsString(arg) || concolic_is(arg),
           "a JS-context arrival was recorded for a value ECMAScript §19.2.1.1 PerformEval step 2 hands back "
           "unevaluated — no program is compiled and there is no §12 lexical state for a breakout to escape "
           "from, so this would raise `reached` for a call that is not a code-execution sink at all");
    g_sink_reached++;
    if (!concolic_is(arg)) return;
    g_sink_tainted++;

    shape = concolic_shape_c(arg);
    src   = concolic_src_c(arg);
    root  = concolic_root_c(arg);

    DCHECK(root != NULL,
           "an attacker value reached a sink carrying no delivery ROOT — the reproduction envelope is built "
           "from it, and without one this finding would state that nothing carries these bytes to the victim");
    /* AND WHETHER AN ATTACKER CAN BE THE ONE STANDING HERE — §Attacker-sources' unforgeable-origin rule, asked
     * at the ONE point all three sink classes converge and at the last moment the flow that arrived still
     * exists.
     * WHAT IS SUPPRESSED IS THE SEARCH, NOT THE ARM. This flow is a real code path and goes on running: the
     * page's handler really does reach `eval` when a message from the origin it pinned arrives, and pruning
     * the arm would delete every endpoint and every value behind it. What it may not do is OPEN A SEARCH,
     * because a search here ends in a fire-verified PoC whose delivery no cross-document attacker can perform
     * — §S's PoC is "the strongest working input per sink", and a PoC that cannot be delivered is a false one.
     * IT IS NOT A "SAFE" VERDICT AND EMITS NO ENTRY, which is the distinction §@S draws: the parked shape says
     * "searched this far and not solved", and saying that here would report a sink an attacker cannot reach as
     * one whose search is merely unfinished. The sink stays reportable through any OTHER flow that reaches it
     * without the demand — the false arm of the equality, or a sibling whose gate was a prefix check, which
     * pins nothing and is exactly the forgeable case the rule SOLVES. */
    /* COUNTED, BECAUSE THIS RETURN IS A DECISION AND NOT AN ABSENCE. Every other silence on this surface means
       the engine did not get here; this one means it got here, did the work, and concluded that no
       cross-document attacker can stand where this flow is standing. Reporting it as the same empty array
       describes the engine's strongest negative result as if it had never looked. */
    if (concolic_principal_pinned()) { g_sink_suppressed++; return; }
    add_pending(src ? src : (shape ? shape : "?"), root, cls);
}

/* DOES THIS SEARCH STILL SEED? — asked by the two callers that would otherwise each spell the answer, and the
 * re-injection point IS the answer rather than a proxy for it. A search holds a path from the moment detection
 * opens it (add_pending) until record_sink CLOSES it at the fire, and the two things record_sink does there are
 * one statement: it releases the path because "a solved search seeds no further candidates", and this is what
 * makes that sentence true instead of hoped. The third door agrees without an exception being written for it —
 * a cold-resumed entry (solve_resume_candidate) holds no path OF ITS OWN and seeds nothing through this file,
 * because its candidates come back as FLOWS rather than as payloads.
 * THAT IS A STATEMENT ABOUT THE RESUME AND NOT ABOUT THE ENTRY, and the difference is the whole of what
 * add_pending's `opened` latch fixed. A resumed entry acquires a path the moment an EXPLORATION flow of this
 * session detects the same sink — which is a detection like any other and opens the search like any other —
 * and from then on it seeds, derives and reports exactly as one opened by detection first. What it never has
 * is a path taken by the resume itself, because a verifying flow does not detect. Read as a claim about the
 * ENTRY, this sentence said a resumed search is inert for the life of the session, which is precisely the
 * wrong verdict `opened` exists to stop.
 * IT IS NOT A SEEN-SET AND TRUNCATES NOTHING. What closes a search is EMITTED OUTPUT — a fire-verified PoC for
 * this exact (source, sink) — which is the one thing §NO BOUNDS allows to prove a flow is done; record_sink
 * already discards a duplicate PoC for the pair at its own top, so what is declined here is a whole document
 * re-run whose only possible result is that discard. Every arm still runs, still arrives and still fires. */
static int search_seeds(const Cand *e) {
    DCHECK(e != NULL, "the seeding question was asked of no search");
    return e->reinject != NULL;
}

/* THE CONTEXT PROBE CAME BACK — the observation §@S(2) asks for, and the reason a derived sink's breakouts are
   not a list. This flow injected the inert locator at the source instead of a breakout, so the string handed to
   the sink is what the page's OWN code built around the attacker's bytes: its concatenations, its filter, its
   re-encoding, all of them run. The class's own parser reads the state each surviving occurrence sits in and
   constructs that state's minimal escape; every escape joins THIS sink's search and the next drain seeds it. */
static void queue_derived(void *user, const char *breakout) {
    Cand *e = (Cand *)user;

    /* A CLOSED SEARCH TAKES NO MORE BREAKOUTS. The probe's OTHER arms keep arriving at this sink after a fire,
       so without this a breakout appended then was seeded on the next drain with the path already released. */
    if (!search_seeds(e)) return;

    /* THE CAPTURE IS NOT HERE, AND THE ASSERT IS WHAT SAYS SO. This used to hold `if (!e->reinject) e->reinject
       = decide_freeze_path();` — the search's ONLY path, taken at the moment a derivation happened — and that
       is the site that has moved to add_pending, because a re-injection point is a fact about the search rather
       than about the derivation and the DETECTING flow already stood at this sink holding it. A second capture
       here would leak the first blob's segment reference and would replace a path that reached the sink with
       another that also did, which is not an improvement to trade a leak for; with one capture there is no
       second reference to release and no ordering between two of them to get wrong. */
    DCHECK(e->reinject != NULL,
           "a breakout was derived for a search that holds no re-injection point — add_pending takes one at the "
           "moment the sink is detected, so a search reaching a derivation without one was opened by some other "
           "door, and this breakout would be seeded to re-search the document's whole gate tree for an arm the "
           "detection and the probe have each already taken");
    /* THE TWO-SIDED HALF OF THE CONSTRAINT. The derivation is handed this search's table and constructs within
       it (solve_html.c / solve_js.c decline at their own emitters), so a breakout arriving here carrying a
       byte the table says does not deliver is a derivation that read the constraint and ignored it — and what
       it costs is a whole document re-run whose only possible outcome is the candidate arriving percent-encoded
       at its own sink. Asserted rather than filtered here, because a filter at this end would let the two
       sides disagree silently and leave the derivation constructing escapes nobody ever sees declined.
       AND THIS IS NOT THE LAST WORD ON THE QUESTION, WHICH IS THE HALF THAT USED TO BE MISSING. The table is
       MEASURED and narrows after this line has run, so an escape pushed while everything still delivered can be
       one this same search later contradicts — and the entry stayed in `pl`, was seeded a full document re-run,
       arrived, and took the arrival rung with it. Deliverability is therefore asked again at the two moments it
       can have changed under a queued spelling: at the seeding, where the payload becomes a flow
       (solve_seed_candidates withdraws it), and at the arrival, where the ledger would otherwise pay for it
       (breakout_arrived). Same constraint, same table, three sites — because the constraint is a measurement
       and a measurement asked once is a constraint that expires. */
    DCHECK(solve_delivered_ok(&e->deliv, breakout),
           "a derived @S breakout carries a byte this search has OBSERVED does not reach its sink — the "
           "derivation is given the same table and emits nothing outside it, so this escape was constructed "
           "past the constraint and would spend a document re-run to arrive transformed");
    push_breakout(e, breakout, CAND_ESCAPE);
}

/* THE SEARCH THE RUNNING CANDIDATE BELONGS TO — asked by BOTH halves of a verifying run, the context probe
   that DERIVES and the breakout that FIRES, because both are the same question. These assertions are about the
   CANDIDATE MACHINERY and not about any parser, so they are stated ONCE rather than re-written per class,
   which is how a second class would otherwise end up with one of them subtly different.
   NULL MEANS THIS SINK IS NOT THIS CANDIDATE'S, and that is a PARTITION and not a swallowed condition. A flow
   carries ONE substitution for ONE (source, sink class), and one source can feed two classes: the full fixture
   writes `location.hash` into an eval sink and, two statements later, into a markup sink. So the eval
   candidate's `';X9()//` arrives at the markup write as well. Deriving there would file breakouts against a
   search that never asked for them — which the class assertion has always said, as an abort — and FIRING there
   would record a MARKUP breakout under the EVAL class: a finding whose sink is not the sink it fired at, which
   §@S(d) cannot reproduce and a reader cannot tell from a real one. Nothing is lost by declining: the other
   class has its own search over that very write, with its own derived breakouts.
   It was an abort because only the probe half could reach it. The firing half can, so the answer is the
   partition rather than the crash — the same shape as `concolic_is(arg)` above it. */
/* DERIVE THIS SEARCH'S BREAKOUTS FROM THE WITNESS IT ALREADY HAS — §12 for the eval sink, §13.2.5 for the
   markup one, the SAME observation read by the parser that owns the sink's language, under the SAME
   constraint table.
   ONE ENTRY AND TWO CALLERS, WHICH IS THE POINT. The first caller is the context probe arriving: the witness
   is stored and this runs on it. The second is a DELIVERY OBSERVATION CHANGING the table, and it runs on the
   witness already stored — that is §@S's "a near-miss is mutated toward the gap using byte-provenance",
   performed by re-deriving rather than by a mutation table, so every escape it produces is still the state's
   own exit transition and no payload is invented. There is no retry counter and no second search: what comes
   back joins this search's list (push_breakout dedups by text), the next drain seeds it as an ordinary flow of
   the one frontier, and a tightened table that yields no new spelling pushes nothing at all — the search then
   starves in the WFQ rather than being stopped by anything.
   ROUTED ON THE CLASS'S OWN DERIVATION COLUMN, the same column add_pending picks the probe from, so a class
   that reaches here with neither parser CRASHES rather than silently deriving nothing. */
/* THE STRING A CONTEXT PROBE'S RUN HANDED THIS SINK, KEPT SO THE DERIVATION CAN BE RE-RUN ON IT. Deduped by
   text: a page that renders the same template twice hands the same witness twice, and re-deriving it can only
   produce breakouts the search already holds. */
static void learn_witness(Cand *e, const char *out) {
    DCHECK(e != NULL && out != NULL && *out,
           "a sink search was handed a context witness with no bytes in it — the witness is what a state is "
           "read off, and an empty one would derive a context for a write that never happened");
    /* AND THE SEARCH HAS DEMONSTRABLY HELD THE THREAD, which is what makes `turns` a measurement rather than a
       number beside one. A witness is produced by a CANDIDATE flow reaching this sink, and solve_flow_begin
       raises `turns` at every switch-in of one, so a witness standing beside `turns:0` means the counter is not
       counting the flow that did the work — and `reached:0,turns:0` would then be read as "the WFQ never
       scheduled this search" for a search whose probe has already run the whole document. That misreading is
       not hypothetical: it is the one the @S half was diagnosed with twice, in both directions, and the pair
       exists precisely so the scheduling question and the distance question can be told apart. */
    DCHECK(e->turns > 0,
           "a sink search learned a context witness while the scheduler has given it no turn — the witness is "
           "produced by one of this search's own candidate flows reaching the sink and solve_flow_begin counts "
           "a turn at every switch-in of one, so `turns` is not counting the flow that produced this and the "
           "parked record's scheduling half is reporting about a different quantity than it names");
    for (int i = 0; i < e->nwit; i++) if (!strcmp(e->wit[i], out)) return;
    if (e->nwit >= e->witcap) {
        e->witcap = e->witcap ? e->witcap * 2 : 4;
        e->wit = realloc(e->wit, (size_t)e->witcap * sizeof(char *));
        CHECK(e->wit, "solve: OOM recording a sink search's context witness");
    }
    e->wit[e->nwit] = strdup(out);
    CHECK(e->wit[e->nwit], "solve: OOM recording a sink search's context witness");
    e->nwit++;
}

/* …AND WHAT IT ANSWERED IS READ. Both derivations RETURN how many escapes they constructed and this call was
   discarding it — a computed value with no reader, which §@S names as the mirror of the read-with-no-writer
   defect and calls "not a mechanism". What the number closes is a silent drop: a breakout the derivation
   CONSTRUCTED and the search never received leaves a list of nothing but instruments, which the report states
   as `probes == payloads` — read as "this search has built no escape", the state a reader takes to mean the
   source cannot carry an exit from the state its bytes are in. queue_derived has exactly one door that drops
   (a search closed by a fire between the derivation and
   the push), and a search that fired holds the breakout that fired it, so the implication holds with no
   exception written for it. */
static void derive_from_witness(Cand *e) {
    int derive, built = 0;

    DCHECK(e != NULL && e->nwit > 0,
           "a derivation was asked to run on a search that holds no witness — the witness is the string the "
           "context probe's own run handed this sink, so without one there is no observation to read a state "
           "off and the re-derivation would be a static shape of the expression");
    derive = sink_class(e->sink)->derive;
    /* THE ASSERT STAYS A `DCHECK` AND THE DISPATCH BELOW STOPS BEING AN `else`, WHICH ARE TWO DIFFERENT
       ANSWERS TO THE SAME OBJECTION. The state this guards is a fact about the SINKS table, and that table is
       a static of this build whose columns sink_declare_check already asserts against each other — a class
       carries written-down vectors XOR a derivation — so a violation here is caught once at startup and is
       not something a page can reach. Dev-only is therefore right, and promoting it would put an
       always-fatal abort on a table this build cannot change while it runs.
       WHAT WAS NOT RIGHT IS WHAT THE RELEASE BUILD DID NEXT. The loop's `else` answered two questions with one
       branch — "is this JS" and "is this not HTML" — so with the assert compiled out a class with NO routed
       parser was handed to the JS derivation, and ECMAScript §12's lexical-state escape was constructed for
       bytes that landed in an HTML tokenizer state. That is a derivation running on a context nothing said it
       reads, and its output is a payload, not a blank: §@S's fire-verification bounds what such a candidate
       can be REPORTED as, and it does not stop the search spending its runs on escapes derived for the wrong
       grammar. Naming both arms costs nothing and makes the unrouted class construct nothing, which is the
       state `built == 0` below already describes. */
    if (derive != SINK_DERIVE_HTML && derive != SINK_DERIVE_JS)
        DFAIL("a sink class stored a context witness and declares no derivation to read it with — a class "
              "whose breakouts are written down never stores one, so this is a class whose derivation column "
              "was set without a parser being routed for it");
    for (int i = 0; i < e->nwit; i++) {
        if      (derive == SINK_DERIVE_HTML) built += solve_html_breakouts(e->wit[i], &e->deliv, queue_derived, e);
        else if (derive == SINK_DERIVE_JS)   built += solve_js_breakouts(e->wit[i], &e->deliv, queue_derived, e);
    }
    DCHECK(built == 0 || cand_has_escape(e),
           "a derivation constructed an escape that this search does not hold — the constructed count and the "
           "search's own payload list are the two ends of one hand-off, so a search left holding nothing but "
           "its probes after a derivation that built something has DROPPED it, and the report would state "
           "`probes == payloads` — which a reader takes as the positive statement that this source can carry no "
           "exit from the state its bytes landed in");
}

/* WHICH OF THE BYTES THIS SOURCE'S COMPONENT PERCENT-ENCODES ACTUALLY REACHED THE SINK — §@S(2)'s
   character-provenance observation, taken off the delivery probe's own run.
   THE ANSWER IS PER BYTE AND IT IS READ, NOT INFERRED. The probe wrote `apiclientbytesK` immediately in front
   of the K'th byte of the declared set, so the character after that token in the string a REAL re-execution
   handed a REAL sink is either that byte or it is not, and no alignment has to be guessed through whatever the
   page did to the rest. Every way of not being it is one answer: `%3C` because the browser encoded it and the
   page did not decode, nothing because a filter dropped it, `&lt;` because the page escaped it. §@S(2) lists
   those forms for the MUTATION's benefit, and solve_filter.h states the reason they collapse here — a byte the
   page re-encoded cannot break a sink out of its context, so for a constraint it is exactly "did not arrive".
   A TOKEN THAT DID NOT ARRIVE SAYS NOTHING, and that is the sound-only direction: a page that truncated the
   probe before token K has told us nothing about byte K, and clearing it on that evidence would decline an
   escape that would have fired. Uncertainty keeps the arm.
   AND A CHANGE IS WHAT DRIVES THE MUTATION. The table narrows monotonically, so it either narrowed — in which
   case the derivation is re-run on the witness and whatever spelling the tightened constraint permits joins
   the search — or it did not, in which case nothing happens at all.
   A NARROWING ALSO WITHDRAWS, AND THAT IS NOT PERFORMED HERE ON PURPOSE. Every spelling already in `pl` was
   pushed under a table this line has just replaced, so some of them are now contradicted arms — but WHICH
   entries those are is a pure function of `pl` and this table, with no moment attached, so recomputing it at
   the two sites that would otherwise spend something on one (solve_seed_candidates, breakout_arrived) keeps
   ONE source of truth. A withdrawal flag written here would be a second copy of an answer this table already
   gives, free to be behind it — and it would have to be written for every search sharing the source rather
   than only the one whose probe happened to run. Monotonicity is what makes that safe: a byte never becomes
   deliverable again, so an entry withdrawn at one reading is withdrawn at every later one. */
static void observe_delivery(Cand *e, const char *out) {
    const char *enc;
    size_t tl = sizeof SOLVE_BYTES_LOCATOR - 1, n, i;
    char tok[sizeof SOLVE_BYTES_LOCATOR + 1];
    int changed = 0;

    DCHECK(e != NULL && out != NULL, "a byte-delivery observation was taken for no search, or off no string");
    enc = concolic_source_encodes(e->root);
    DCHECK(enc != NULL && *enc,
           "a delivery probe reached a sink for a search whose source declares no percent-encode set — the "
           "probe is BUILT out of that set (add_pending), so a search running one without a declaration was "
           "seeded a payload nothing in this file constructs");
    /* THE ARRIVAL IS COUNTED BEFORE ANY TOKEN IS LOOKED FOR, which is the whole of what separates this
       observation's ABSENCE from its ZERO — the same sentence filter_survived states one rung up about
       `sinkStrings`, and for the same reason. Reaching this line IS the delivery probe's bytes turning up in a
       string a sink was handed: the caller partitioned on the probe's own locator to get here, so the fact is
       already established and none of the scan below can retract it. A run that finds nothing is the LOUDEST
       result this instrument has — the page destroyed the probe — and counting it only on success would report
       it as the search never having been scheduled. */
    e->deliv_runs++;
    n = strlen(enc);
    memcpy(tok, SOLVE_BYTES_LOCATOR, tl);
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)enc[i];
        const char *p;

        tok[tl] = (char)('0' + (int)i); tok[tl + 1] = 0;
        if (!(p = strstr(out, tok))) continue;          /* this token never arrived: says nothing about the byte */
        /* THE PROBE'S OWN TOKEN GOT HERE, so whatever stands behind it is an OBSERVATION of that byte — which
           is the fact `deliv_seen` states and which the permissive initial table cannot. */
        e->deliv_seen = 1;
        if ((unsigned char)p[tl + 1] == b) continue;    /* delivered as itself */
        if (!e->deliv.ok[b]) continue;                  /* already observed, and the table only narrows */
        e->deliv.ok[b] = 0;
        changed = 1;
    }
    if (changed && e->nwit > 0) derive_from_witness(e);
}

/* A BREAKOUT OF THIS SEARCH JUST ARRIVED AT ITS OWN SINK — the ONE place `reached` moves, so the three sink
   classes cannot come to disagree about what the number counts, which is exactly how it came to count the
   context probe in two of them and not in the third.
   THE ASSERTION IS THE IMPLICATION `reached` NO LONGER RESTATES. A derived class's breakout exists only
   because its context probe ran and returned one, so a search holding nothing but its own probe cannot have
   produced the bytes that just arrived — and a single-context class states its vectors at detection and has no
   probe, so it is exempt BY CONSTRUCTION rather than by an exception written into the condition. */
static void breakout_arrived(Cand *e) {
    /* THE BYTES THAT ARRIVED ARE THE RUNNING FLOW'S OWN SUBSTITUTION, so they are asked of that flow and not
       of the sink. It is the SAME flow whose `cand_src`/`cand_sink` resolved `e` one call up (candidate_search),
       and filter_survived has already CHECKed the triple on the same string — asking anywhere else would be
       asking about a different flow's bytes, which is the mistake the search-level ratchet makes by
       construction and the reason `survivedBy` had to exist. `CHECK` because it is dereferenced below. */
    Flow *f = flow_running();

    DCHECK(e != NULL, "a breakout arrived at no search — the caller resolved one before reading the bytes");
    CHECK(f != NULL && f->cand_payload != NULL,
          "solve: a breakout arrived at a sink with no candidate substitution on the running flow — nothing but "
          "a candidate run injects a marker, and the rung below is paid on whether THIS search still holds "
          "these exact bytes as viable, so an arrival with no payload cannot be told from one it withdrew");
    /* WHAT ARRIVED IS ASKED OF THE BYTES, NOT OF A COUNT — and that is the whole of the fix rather than a
       restatement. The check used to be `npl > nprobe`: "this search holds at least one entry past its leading
       probes". That is a claim about the LIST, and it stands in for the claim anybody actually wants, which is
       about THESE BYTES: could this search have produced the thing that just turned up? The two agree only
       while every candidate of a search has a row in that list, and §@S has one kind that legitimately does
       not — a candidate resumed out of the cold tier, whose payload rides the resumed FLOW (solve.h, on
       `payloads` being empty beside a non-zero `tried`). Such an entry stands at `npl == 0`, so the list claim
       read FALSE for a candidate that had replayed its recorded path, delivered its payload and carried it all
       the way to its own sink — the search's furthest arrival aborting the process on a check about a list it
       was never in.
       THE THREE STATES, AND EACH IS DECIDED BY THE ENTRY'S OWN LABEL:
         CAND_ESCAPE  this search built or stated these bytes as an attack — the ordinary case, in either
                      order the two producers arrive in, because the label is pushed with the entry.
         0            this session's record has no row for these bytes, which the cold tier is the one
                      legitimate producer of and which `cand_resumed` STATES rather than leaving to be
                      inferred from an absence.
         CAND_PROBE   the bytes that arrived are one of this search's own INSTRUMENTS. That is what this check
                      is for and what it still catches: a probe is inert by construction and carries no X9, so
                      the marker partition that routed this arrival has broken, and the ladder is about to pay
                      an arrival rung — and, at the URL class, a FIRE — for a measurement rather than an
                      attack. It was previously caught only in the special case where the probe was ALSO the
                      only thing in the list. */
    DCHECK(cand_arrival_is_attack(e, f),
           "a sink recorded a BREAKOUT arriving whose bytes are not an attack of this search — they are "
           "either one of its own inert PROBES, which carries no marker and cannot fire, so the partition "
           "that routed this arrival has broken and a measurement is about to be paid an arrival rung; or "
           "they are bytes this session's record does not hold and the flow carrying them was not rebuilt "
           "from a park record, which means a candidate was assembled outside both of the search's doors "
           "(solve_seed_candidates and solve_resume_candidate) and nothing knows what it is running");
    /* …AND THE SAME PRECONDITION filter_survived STATES, at the rung above it. The marker is a strong
       partition and not a proof: `X9` is two characters and a minified bundle names things that way, so an
       arrival credited without this is a whole rung — and, at the URL class, a FIRE — taken on the page's own
       address. The entry asked already; this is what keeps that true of the next entry. */
    DCHECK(concolic_candidate_delivered(),
           "an @S breakout was recorded as ARRIVING for a flow whose payload has not entered the program — "
           "the marker these bytes were found by is the page's own, so the ladder is about to advance a "
           "candidate for a string it never produced");
    /* …AND THE SAME QUESTION ASKED OF THIS FILE'S OWN RECORD, for the reason filter_survived states about its
       copy: the line above is the COMPONENT's live answer and this is whether the event was ever reported
       here, so a second door into the substitution shows up as an arrival standing over `substituted:0`
       rather than as a report that quietly says the runway was never left. */
    DCHECK(e->substituted > 0,
           "an @S breakout arrived at its own sink for a search that has never recorded a substitution — the "
           "bytes got here, so they entered the program, and concolic_deliver reports every entry to "
           "solve_observe_substitution; the parked card would state that these runs never reached their own "
           "source read");
    /* §@S's SECOND FITNESS RUNG, WRITTEN TO BOTH QUANTITIES — "the search is DISTANCE-DIRECTED (a fitness of
       {filter-survived, sink-reached, context-escaped, handler-fires} the WFQ reads)". A candidate flow records
       no endpoints by design (endpoint_suppress), so before this rung existed the ONLY thing that could ever
       raise a candidate's reward was record_sink — the LAST rung, a
       fire-verified PoC. A candidate that carried the attacker's bytes all the way to the sink and did not
       break out of it was worth exactly as much to the scheduler as one that had not started, so the near-miss
       §@S says to mutate toward the gap was outranked by every arm of the exploration tree and never ran again.
       AND THE LEDGER ALONE DID NOT CLOSE THAT, WHICH IS WHY THE RUNG IS NOW WRITTEN TWICE. A crossing is paid
       once per SEARCH, so the second candidate to reach this sink was paid nothing for reaching it — and on a
       page that does not transform the payload the survival fraction is 1.0 for every candidate of the search
       the moment its bytes surface anywhere, so the comparator was a CONSTANT across exactly the population
       §@S needs ordered. The arm that arrived and the arm that never left the runway compared equal, and "a
       near-miss is mutated toward the gap" had nothing to read. flow_observe_rung is the other half: the
       ARRIVAL is now a fact recorded on THIS FLOW, so every candidate that reaches this sink outranks every
       candidate of the same search that has not, however many got here first.
       ONCE PER SEARCH, at the 0→1 crossing, and that is what "NEW" means in §WFQ's "accumulated emitted VALUE
       (new @H+@S)" — the same shape sink_search uses one function up, where only the call that CREATED the
       search credits it. It is not a seen-set and truncates nothing: every later arrival still derives, still
       fires and still raises `reached`; what is not repeated is the CREDIT for an observation already made.
       AND IT IS NOT PAID TO AN ARM THIS SEARCH HAS ALREADY CONTRADICTED — which is the whole of what the
       latch beside `reached` is for. queue_derived asserts deliverability at PUSH time and the table narrows
       afterwards, so a breakout constructed under the permissive table and seeded before the delivery probe
       came back still arrives here. Its bytes really are in the string (the marker survived; the escape did
       not), so `reached` counts it — but the RUNG is a distance to firing, and a candidate the search's own
       measurement has since ruled out has travelled none of that distance. Paid to it, the one-time crossing
       was then unavailable to the spelling that CAN fire: measured on the attribute-context markup sink, where
       two of three constructed escapes carry the marker into an attribute NAME (`%3e%3csvg%20onload=`) and can
       never reach a handler, while the third lands in a real `onerror` and does.
       THIS IS PRUNING A CONTRADICTED ARM AND NOT A BOUND. Nothing here counts runs, ages a candidate, or
       remembers that it has been seen; the ONE question is whether this search has POSITIVELY OBSERVED that
       these bytes do not arrive (solve_filter.c narrows only on a token the delivery probe's own run put in
       front of the byte, so uncertainty keeps the arm). It is the same constraint solve_html.c and solve_js.c
       construct under and the same one queue_derived asserts — asked again at the moment the ledger would
       otherwise spend on it, because the table is not the table it was asked against.
       A CROSSING ALREADY PAID IS NOT TAKEN BACK. If the arriving candidate was viable when it crossed and the
       table contradicted it later, the payment stood for an observation that was TRUE when it was made, which
       is the whole rule a ledger obeys; un-paying it would revise a record on hindsight and would charge
       whichever flow happens to be running now for a credit some other flow was given. What the search loses
       there is one rung, once, and the COMPARATOR still separates the two candidates — a contradicted spelling
       is refused the arrival rung as well as the crossing, so the spelling that can still arrive stands a whole
       rung above it and holds that lead for the rest of its life rather than for one payment. */
    /* ONE QUESTION, TWO WRITES, AND THE PAIR IS WHAT §@S(ii) IS ABOUT. The condition is the search's own
       measurement that these bytes can still arrive; inside it the COMPARATOR is written unconditionally and
       the LEDGER is latched, which is the whole of the difference between the two quantities stated at the one
       site that makes both. The rung says where THIS flow stands and is therefore true of the second, third
       and tenth candidate to stand there; the crossing says what the SEARCH learned and is therefore paid to
       the first only. */
    if (solve_delivered_ok(&e->deliv, f->cand_payload)) {
        flow_observe_rung(f, FLOW_RUNG_ARRIVED);
        if (e->reach_credited == 0) { e->reach_credited = 1; flow_credit_emit(1.0); }
    }
    e->reached++;
}

/* §@S's FIRST *SINK* FITNESS RUNG — "filter-survived" — AND THE ONLY ONE OF THE THREE THAT IS CLASS-INDEPENDENT.
   The other two report at or past the sink of the candidate's OWN class, so a flow that had not got
   there yet was worth nothing whatever it had done at any other sink. This one is asked of EVERY string that reaches ANY
   code-execution sink during a candidate run, before the class partition, because that is what the question
   is: not "did this breakout arrive at its sink" but "how much of what the page was given is still alive".
   A candidate for the eval sink whose bytes turn up at a markup write has demonstrably survived the page's own
   filter and travelled through its code — §@S(2)'s "moved" — and until this line that observation was
   discarded by `candidate_search` returning NULL.
   AND IT IS NO LONGER THE BOTTOM OF THE LADDER. Every rung here is measured on a STRING A SINK WAS HANDED, so
   a candidate still on the runway stood at 0 whatever it had done — §@S(i)'s objection, applied to the rungs
   that replaced the outcome-restating ones. FLOW_RUNG_DELIVERED is below all of them and is observed at the
   SOURCE READ, in the component that performs the substitution (solver/concolic.c), which is the only site
   between a source read and a sink write that can state anything about this flow's bytes without a taint
   tracker. So the three-state zero this file's entries produce is now a genuine partition: rung 0 is a flow
   whose bytes never entered the program, rung 1 with `cand_surv == 0` is one whose bytes entered and did not
   survive to any sink, and a nonzero fraction is this measurement.
   THE SEARCH IS THE FLOW'S OWN, not the sink's. A flow carries one substitution for one (source, sink class),
   so the progress belongs to that search wherever the bytes surface; asking the SINK which search to credit
   would credit whichever one happens to own the write.
   IT IS A `CHECK` FOR THE REASON record_sink STATES ABOUT ITS OWN TWO: the pointer is dereferenced below, so a
   DCHECK alone turns a dev-mode abort into a release segfault. A candidate flow exists only because detection
   opened its search, and solve_flow_begin asserts the same entry on every switch-in.
   NO CREDIT FOR GROUND ALREADY PAID FOR. The pair on the entry is the BEST any candidate of this search has
   reached; an observation that does not beat it pays nothing, and one that does pays exactly the fraction it
   added. So the rung is worth at most 1.0 over the search's whole life, it can never be re-earned, and it
   truncates nothing — every arrival is still measured, still derives, still fires. */
static void filter_survived(const char *out) {
    Flow *f = flow_running();
    FilterObs o;
    Cand *e;
    double had, now;

    CHECK(f != NULL && f->cand_src != NULL && f->cand_payload != NULL && f->cand_sink != NULL,
          "solve: a string reached a code-execution sink inside a candidate run while the running flow holds "
          "no substitution — this rung measures the sink's output against the bytes THIS flow injected, so a "
          "flow with none is not a candidate at all, and all three are dereferenced immediately below");
    e = search_of(f->cand_src, sink_class_of_name(f->cand_sink));
    CHECK(e != NULL,
          "solve: a candidate flow's bytes reached a sink for a search this session has no entry for — the "
          "candidate exists only because detection opened one and a cold-resumed one re-registers before it "
          "runs an opcode, so an absent entry is a search dropped under a live flow");
    /* THE MEASUREMENT'S OWN PRECONDITION, ASSERTED WHERE THE MEASUREMENT IS AND NOT ONLY WHERE IT IS ROUTED.
       Every candidate arm returns before reaching here unless this flow's substitution has been performed
       (concolic.h), and asserting it again is what makes that STRUCTURAL: a fourth sink class, or a fourth
       route into an existing one, cannot quietly re-open the door that let this rung measure the page's own
       strings. Without it the failure is silent by construction — a nonzero fraction of a real payload found
       in a real string, which is indistinguishable from an observation.
       IT IS NOW HALF OF A PAIR AND THE HALVES ARE NOT THE SAME CLAIM. This one asks the COMPONENT: are these
       bytes in the program of the replay running right now — the answer a restart resets. flow_observe_survival
       asserts the FLOW's ladder rung: has this flow ever been observed to deliver at all — the answer §@S
       requires monotone. A restarted candidate makes them differ, and the sink entry's own return is what
       keeps this one true; neither implies the other, and a reader who deletes either as a duplicate has
       deleted the only statement of one of the two. */
    DCHECK(concolic_candidate_delivered(),
           "an @S survival fraction is about to be measured for a flow whose payload has not entered the "
           "program — the run this is about to find is the PAGE'S own bytes coinciding with the candidate's, "
           "so the flow's fitness, the search's ratchet and the report's surviving-byte count would all be "
           "readings of text the attacker never supplied. The sink entry that reached here did not ask "
           "concolic_candidate_delivered");

    /* THE OBSERVATION IS COUNTED BEFORE IT IS MADE, which is the whole of what separates its ABSENCE from its
       ZERO. Everything below this line is a best-so-far: the fraction is taken, the ratchet keeps it only if
       it improves, and a run of zero returns without writing anything anywhere — so a search that has watched
       four hundred sink writes go by carrying none of its bytes reported exactly what one that has never seen
       a sink reports. §@S names that tell by name. Raised HERE, above the `run == 0` return and above every
       improvement test, because the fact being recorded is that a code-execution sink RAN while this search's
       substitution was live, which is true of the zero observation exactly as much as of the best one.
       NOT A CREDIT AND NOT A RUNG: flow_credit_emit is not called, no crossing is latched, and the flow's own
       comparator is written further down by flow_observe_survival. This is the REPORT's counter, and §@S(ii)
       is why it is a different quantity in a different place from both of them.
       AND IT ASSERTS THE COUNT BELOW IT, which is what keeps the delivery report from going missing. A string
       reaches this line only with the substitution live in THIS replay, and every substitution is performed at
       concolic_deliver, which reports it to solve_observe_substitution — so a sink observation standing over
       `substituted:0` is a second door into the substitution that never told this file, and the parked card
       would then state that these runs never reached their own source read about bytes it is measuring inside
       a sink's own string. `concolic_candidate_delivered` above asks the COMPONENT for its live answer; this
       asks whether the event was ever RECORDED here, and neither implies the other. */
    e->sink_strings++;
    DCHECK(e->substituted > 0,
           "an @S candidate's bytes reached a code-execution sink for a search that has never recorded a "
           "substitution — every delivery runs through concolic_deliver and is reported to "
           "solve_observe_substitution, so this is a second door into the substitution that does not report "
           "one, and the parked card is about to state that these runs never reached their own source read");

    /* AND THE DELIVERY PROBE IS READ HERE, at the same class-independent point and for the same reason this
       function already gives about its own rung: the observation is about the RUNNING FLOW'S OWN bytes, so it
       is true wherever they surface and belongs before the class partition rather than inside one sink. The
       token is the partition — a probe built out of the source's percent-encode set carries it and nothing
       else does, exactly as the context locator partitions the derivation from the breakout. */
    if (!strncmp(f->cand_payload, SOLVE_BYTES_LOCATOR, sizeof SOLVE_BYTES_LOCATOR - 1))
        observe_delivery(e, out);

    solve_filter_survival(out, f->cand_payload, &o);
    DCHECK(o.len > 0, "a candidate flow carries an empty payload — see solve_filter.c's own assert");
    /* THE FITNESS, WRITTEN WHERE A FITNESS IS WRITTEN — on THIS FLOW, before the search-level ratchet, and as a
       reading rather than a payment. The ratchet below is the LEDGER: it records what this SEARCH has learned
       and is therefore paid at most once for one distance, which is what keeps the reward term honest and is
       exactly what makes it unable to order two candidates of one search — the second flow to travel nine
       tenths of the way is paid nothing and ranks with a flow that has not started. §@S calls the search
       DISTANCE-DIRECTED and says a dead candidate starves; a ledger cannot express either, because both are
       statements about candidates STANDING at different distances at the same moment.
       SO IT IS TAKEN FIRST AND UNCONDITIONALLY, above the `run == 0` return and above the ratchet's
       improvement test. A zero run is an observation with a value of zero and flow_observe_survival discards it
       as a non-improvement; a run that ties the search's best is no news to the ledger and is the whole news to
       the comparator, because it says THIS candidate is where the best one got. */
    flow_observe_survival(f, (double)o.run / (double)o.len);
    if (o.run == 0) return;                  /* none of this candidate is in this string: an OBSERVATION */
    /* WHICH CANDIDATE THIS IS, recorded before the search-level ratchet because the ratchet is what erases the
       distinction. A NEGATIVE index is a POSITIVE statement and not a miss: solve_resume_candidate raises
       `tried` for a cold-resumed candidate whose payload rides the resumed FLOW rather than this session's
       record, so its bytes are legitimately not in `pl` (solve.h says the same thing about `payloads` being
       empty beside a non-zero `tried`). The search-level pair still records it, so nothing is lost — only the
       per-candidate column, which this session has no row for.
       AND THE INDEX IS FOUND WHEN THIS SESSION HAPPENS TO HOLD THE SAME BYTES, which is correct and not a
       collision: the column is keyed by PAYLOAD and not by flow, so a resumed candidate and a derived one
       carrying the same string are one row that both write — the same reason push_breakout dedups by text. */
    {
        int idx = -1;
        for (int i = 0; i < e->npl; i++) if (!strcmp(e->pl[i].bytes, f->cand_payload)) { idx = i; break; }
        if (idx >= 0) {
            DCHECK(o.len == (int)strlen(e->pl[idx].bytes),
                   "a candidate's payload matched its search's record by text and disagrees with it by length — "
                   "the two are the same bytes by construction, so the per-candidate survival column is about "
                   "to be scaled by a denominator that is not this candidate's");
            if (o.run > e->pl[idx].surv) e->pl[idx].surv = o.run;
        }
    }
    /* Cross-multiplied so the comparison is exact rather than a float one, and so the zero state — no
       observation yet, held as 0/0 — is the one case that falls through rather than comparing against itself. */
    if (e->surv_len != 0 && (long)o.run * e->surv_len <= (long)e->surv_run * o.len) return;
    now = (double)o.run / (double)o.len;
    had = e->surv_len ? (double)e->surv_run / (double)e->surv_len : 0.0;
    DCHECK(now > had,
           "the filter-survival ratchet is about to pay for ground it has already paid for — the comparison "
           "one line up is the whole of what makes this rung worth at most one point, so a credit that is not "
           "an improvement is a value leak that reorders the frontier on noise");
    e->surv_run = o.run; e->surv_len = o.len;
    /* …AND THE OFFSETS OF THE RUN JUST RECORDED, IN THE SAME BRANCH, because the length and the position are
       ONE observation: solve_filter_survival reports them together and its own assert re-reads the bytes at
       them, so a split write would let this search hold a run measured on one string and a position measured
       in another. There is no state in which the pair moves and the offsets do not.
       ASSERTED AGAINST WHAT THE SEARCH NOW HOLDS, which is the half the component cannot check. solve_filter.c
       has already verified that these offsets name this run inside THIS observation's two strings; what only
       this site can say is that the four numbers the SEARCH is left holding are consistent with each other,
       which is the invariant every later reader — the report, and the mutation that reads which segment died —
       actually depends on. */
    e->surv_at = o.at; e->surv_out = o.out_at;
    DCHECK(e->surv_at >= 0 && e->surv_out >= 0 && e->surv_at + e->surv_run <= e->surv_len,
           "the @S survival ratchet recorded a surviving segment that does not fit inside the candidate it "
           "measured, or recorded a run with no position — the offset is into the candidate and the run is a "
           "substring of it by construction, so a segment running past its own end names bytes of some other "
           "string, and the mutation these offsets exist to aim would be aimed at them");
    flow_credit_emit(now - had);
}

/* §@S's THIRD FITNESS RUNG — "context-escaped" — AND IT IS NOT THE SAME FACT AS ARRIVING OR AS FIRING.
   ARRIVING is `reached`: the breakout's bytes are in the string the sink was handed. FIRING is the marker
   actually calling, which only re-execution decides and which §@S accepts as the sole proof. Between them sits
   the question the whole derivation exists to answer — are the bytes OUT of the state they were written into —
   and it had no observation site at all, so a breakout that landed inside the literal it was meant to leave
   and one that reached an executable position and threw were the same number.
   EACH CLASS ANSWERS IT FROM ITS OWN LANGUAGE and none of them re-derives anything: the eval sink asks the
   same §12 scan that built the escape whether the marker now begins an input element, the markup sink reads
   the marker out of an auto-firing handler in the REAL parse it already runs (HTML §8.1.1 Introduction lists
   "event handler content attributes" among the mechanisms that "cause author-provided executable code to
   run"), and the URL sink asks whether the delivered address survived as a `javascript:` one, which for a
   single-context sink IS the escape.
   ONCE PER SEARCH AT THE 0->1 CROSSING for the LEDGER — the shape breakout_arrived uses, and for its reason:
   this is a boolean fact about the search, so it has exactly one crossing and repeating the credit would pay
   twice for one observation. AND ONCE PER FLOW FOR THE COMPARATOR, which is the same split breakout_arrived
   makes and the reason the rung is worth writing at all: the crossing says what the search learned, the rung
   says which of its live candidates is standing at an executable position RIGHT NOW, and only the second can
   order the second and tenth breakout of one derivation against each other.
   THE DELIVERY TABLE IS ASKED HERE TOO, and it is not a second policy: it is the SAME question breakout_arrived
   asks one rung down, and asking it once there would leave this rung able to hand a contradicted spelling the
   whole ladder — two thirds of the comparator's range — one moment after the arrival rung refused it a third.
   The table only ever narrows, so a spelling still deliverable here was deliverable at its arrival, which is
   what keeps flow_observe_rung's ordering assert an invariant rather than a hope. */
static void escape_reached(Cand *e) {
    /* THE FLOW IS THE RUNNING ONE, for breakout_arrived's reason exactly: these are the running flow's own
       injected bytes, and the search `e` is the one its substitution resolves to. `CHECK` because both are
       dereferenced below. */
    Flow *f = flow_running();
    /* ASKED ONCE AND HELD, so the gate below and the assertion after it are about the SAME observation rather
       than two readings of a table that is measured while the search runs. */
    int deliverable;

    DCHECK(e != NULL, "a context escape was recorded against no search — the caller resolved one to record the "
                      "arrival that must precede it");
    CHECK(f != NULL && f->cand_payload != NULL,
          "solve: a context escape was observed at a sink with no candidate substitution on the running flow — "
          "nothing but a candidate run injects a marker, so an escape with no payload behind it is a fact about "
          "bytes this flow did not write and the rung below would be given to whoever happens to be running");
    DCHECK(e->reached > 0,
           "a breakout was observed in an EXECUTABLE position at a sink its own bytes have not been recorded "
           "as ARRIVING at — every escape site runs downstream of breakout_arrived on the same string, so an "
           "escape with no arrival behind it means the two are being asked about different strings");
    /* ONE QUESTION, TWO WRITES — breakout_arrived's split, made at the rung above it. The COMPARATOR is written
       unconditionally inside the gate (this flow stands here, however many stood here first) and the LEDGER is
       latched on a field of its OWN, never on the report counter below: `escaped` counts every arrival at an
       executable position including a contradicted spelling's, and a latch read off it is spent by exactly the
       observation the gate refused to pay for. */
    deliverable = solve_delivered_ok(&e->deliv, f->cand_payload);
    if (deliverable) {
        flow_observe_rung(f, FLOW_RUNG_ESCAPED);
        if (e->escape_credited == 0) {
            /* THE LEDGER'S LADDER IS ORDERED, ASSERTED WHERE IT IS CLIMBED — the ledger's half of the invariant
               flow_observe_rung asserts for the comparator, and the only place it can be asked. It is DERIVED
               and not assumed: this crossing is paid only when the delivery table still holds these bytes, the
               table only ever narrows, so they were deliverable when this same flow ARRIVED — and it did arrive,
               because flow_observe_rung one line up refuses FLOW_RUNG_ESCAPED to a flow that has not stood on
               FLOW_RUNG_ARRIVED. So breakout_arrived's own crossing is already paid, and anything that makes
               that false has made the two rungs answer different questions about deliverability.
               IT IS ALSO WHAT FORCES THE NEXT RUNG TO CARRY ITS PREDECESSOR: a fourth crossing added above this
               one asserts the third here, exactly as this one asserts the second, so a ladder cannot grow a
               rung whose ledger can be paid out of order. */
            DCHECK(e->reach_credited == 1,
                   "the @S escape crossing is about to be paid to a search whose ARRIVAL crossing never was — "
                   "the ladder's rungs are ordered and the ledger climbs them in order, so a search standing on "
                   "the escape rung unpaid for the arrival rung has two crossings asking different questions "
                   "about the same delivery table, and the frontier is ranking a distance nobody travelled");
            e->escape_credited = 1;
            flow_credit_emit(1.0);
        }
    }
    e->escaped++;
    /* AND THE TWO-SIDED HALF, WHICH IS THE ONE THAT CATCHES THE LATCH BEING READ OFF THE COUNTER AGAIN. A
       DELIVERABLE escape leaves this function with the crossing paid, always — there is no state in which the
       search has observed one and still owes it. Written after the increment on purpose: with the latch read
       off `escaped`, the second deliverable escape of a search whose first escape was contradicted arrives
       here with `escape_credited` still 0, and this is the line that says so. */
    DCHECK(!deliverable || e->escape_credited == 1,
           "a deliverable @S breakout stood in an executable position and the search's escape crossing is "
           "still unpaid — the ledger's latch is being read off a REPORT counter, which counts the arrivals "
           "the ledger refuses to pay for, so the first contradicted escape spent the crossing that the "
           "spelling which can actually fire needed");
}

static Cand *candidate_search(int sink) {
    Flow *f = flow_running();
    int created = 0;
    Cand *e;

    DCHECK(f && f->cand_src && f->cand_sink,
           "an @S candidate's own bytes reached a sink outside a candidate flow — nothing but a candidate run "
           "injects them, so a string carrying one was built by something that is not this search");
    if (sink_class_of_name(f->cand_sink) != sink) return NULL;
    e = sink_search(f->cand_src, sink, &created);
    DCHECK(!created,
           "the sink a candidate is running for was not on the pending list — the candidate exists only "
           "because detection put it there, so an absent entry means this search was dropped and what is about "
           "to be derived or recorded has no seeded flow to belong to");
    return e;
}


void solve_eval_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {                          /* candidate run: the arg is the injected+wrapped CONCRETE code */
        Cand *e;
        const char *code;
        if (concolic_is(arg)) return;           /* injection didn't reach this read -> not our candidate */
        /* …AND THE OTHER HALF OF THAT SENTENCE, WHICH THE LINE ABOVE CANNOT SAY. "The injection did not reach
           this read" was being tested by "the value is not concolic", and solve_html_sink's own comment
           already records that this "is also true of every literal the page writes". The classes each patched
           it downstream with a locator/X9 partition, and everything ABOVE that partition — the survival rung,
           which is deliberately class-independent — kept measuring the page's own strings against this
           candidate's payload. A breakout is punctuation, so a run of one or two bytes is found in almost any
           string a page builds: §@S's first rung read NONZERO for candidates whose bytes had never entered the
           program, the search's ratchet paid for it once, and the popup rendered "S of L bytes of the furthest
           candidate survived" about text the attacker never supplied.
           THE FACT THAT ANSWERS IT IS NOT ABOUT THE STRING AT ALL — it is whether this flow's substitution has
           been performed yet (concolic.h), which only the component that performs it can know. A `0` is the
           positive statement that nothing here is this flow's, so there is nothing to measure, nothing to
           partition and nothing to fire: this return is the whole candidate arm's precondition and is why the
           two sibling sinks below carry the same line rather than a gate of their own.
           IT IS NOT A BOUND. Nothing is counted, aged or remembered; the same flow reaching the same sink one
           statement after its source read is measured in full, and a flow that never delivers loses only
           readings that were never about it.
           AND THE RUNG THIS GATE IMPLIES IS NOT WRITTEN HERE, which is the one thing to check before adding a
           write beside this line. §@S(i) wants an observation site strictly before the thing it measures a
           distance to, and this line stands AT a sink: a delivery rung recorded here would separate nothing
           the sink rungs below do not already separate, and would leave "delivered, still on the runway"
           reading exactly 0 beside "never delivered" — the pair the rung exists for. It is written where the
           substitution is PERFORMED (solver/concolic.c), and this gate is what makes a flow that stands on the
           rung but is replaying from the baseline again return here instead of measuring. */
        if (!concolic_candidate_delivered()) return;
        /* A NON-STRING SOURCE IS NOT A SINK, AND THAT IS A POSITIVE STATEMENT RATHER THAN A GUARD.
           ECMAScript §19.2.1.1 PerformEval ( source, strictCaller, direct ) step 2 is "If source is not a
           String, return source" — the argument is handed back UNEVALUATED, so nothing is compiled, no JS
           context exists to break out of, and there are no bytes for the filter rung to measure. `eval(fn)`
           and `eval({})` are the whole of it: an object is NOT coerced here the way it is at a USVString or
           DOMString sink, which is why the sibling sinks below must NOT copy this line — `location = {
           toString(){ return "javascript:…" } }` really does run ToString and really is a vector, so an early
           return there would delete a true finding rather than decline a false one.
           Without this the conversion below reached ToPrimitive on a real object and aborted, which read as a
           missing engine capability when the spec had already answered: there is nothing here to convert. */
        if (!JS_IsString(arg)) return;
        /* THE STRING IS CONVERTED BEFORE THE CLASS PARTITION, which is what puts the filter rung inside the
           runway. The partition below decides whose SINK this is; the survival measurement is about the
           running flow's own bytes and is true wherever they surface, so asking it first is not a reordering
           of the same work — it is the only place the observation exists at all. */
        if (!(code = JS_ToCString(ctx, arg))) return;
        filter_survived(code);
        if (!(e = candidate_search(SINK_EVAL))) { JS_FreeCString(ctx, code); return; }   /* another class's search owns this write */
        {
            /* THE SAME PARTITION THE MARKUP SINK MAKES, and for the same two reasons. Each candidate kind is
               told apart by the bytes IT injects, which the other cannot contain: the CONTEXT PROBE carries the
               inert locator and no X9 (that is what makes it inert), a derived BREAKOUT carries X9 and no
               locator (it is built from the §12 state, not from the probe's token). It is a partition, not an
               ordering heuristic.
               AND IT IS WHAT KEEPS THE PAGE'S OWN EVALS OUT OF THIS SEARCH, which matters more now that the
               ENGINE announces every one of them rather than a fixture announcing the few it staged. A page
               calling `eval` on a literal reaches this line during every candidate flow, and the two tests
               below say exactly what it is: not this search's probe, not this search's breakout, so nothing is
               derived from it and nothing is counted as having arrived. The bytes are still evaluated — by the
               engine, once, because that is what the page's own code does — and this file neither performs nor
               suppresses that. */
            /* ONLY THE BREAKOUT BRANCH IS AN ARRIVAL — see `reached`, and see what counting both cost. */
            /* AND THE BREAKOUT BRANCH FIRES NOTHING, because THE SINK ITSELF IS ABOUT TO. This detector used to
               be reachable only from a fixture whose `eval` was a stand-in that evaluated nothing, so the
               search had to MODEL the evaluation — queue the sink's own argument back onto the flow as another
               program, at DYN_POS_IMMEDIATE to stand for 19.2.1.1 running it before the next statement. The
               engine now announces the REAL 19.2.1 and 20.2.1.1.1, and returns from this call straight into the
               compile, so a queued copy would be a SECOND executor beside the real one: the breakout would run
               twice and record its finding twice. It was also the weaker of the two, in two ways the spec
               names — a DIRECT eval evaluates in the CALLER'S scope with the caller's strictness while a queued
               program is global, so a breakout naming a local fired here and would not fire in a browser; and
               20.2.1.1.1 CreateDynamicFunction CREATES a function without CALLING it, so `new Function(payload)`
               fired here and fires nothing in a browser. Re-execution is the oracle §@S asks for, and the
               engine's own evaluation IS the re-execution. */
            if (strstr(code, SOLVE_JS_LOCATOR))  { learn_witness(e, code); derive_from_witness(e); }
            else if (strstr(code, "X9")) {
                const char *m;
                breakout_arrived(e);
                /* §@S's CONTEXT-ESCAPED RUNG for the JS class, asked of the SAME §12 scan that built the
                   escape. EVERY occurrence is asked, and the first that begins an input element answers: a
                   page that writes the source twice puts the marker in two states, and one of them being an
                   executable position is what "this candidate got out" means. The break is not an early exit
                   past unfinished work — the rung is a boolean about the search, so a second yes says nothing
                   the first did not. */
                for (m = code; (m = strstr(m, "X9")) != NULL; m += 2)
                    if (solve_js_at_source(code, (size_t)(m - code))) { escape_reached(e); break; }
            }
            JS_FreeCString(ctx, code);
        }
        return;                                 /* the marker records the PoC when the engine runs these bytes */
    }
    /* ECMAScript §19.2.1.1 PerformEval ( source, strictCaller, direct ) step 2, "If source is not a String,
       return source" — ASKED ON THE DETECTION ARM TOO, because the seam above is announced UNCONDITIONALLY
       (js_eval_program_source announces every source before it decides anything) while `reached` is defined in
       solve.h as how many times a code-execution sink was EXECUTED. `eval(fn)`, `eval({})` and `eval(42)`
       execute nothing: step 2 hands the argument straight back, no program is compiled, and there is no §12
       lexical state for a breakout to escape from — so counting one raises the number whose ONLY job is to
       make an empty `@S []` attributable, on a call that is not a sink.
       WHAT THAT COSTS IS A WRONG READING AND NOT A WRONG DIGIT, WHICH IS WHY IT IS WORTH A LINE. solve.h's
       four states each take a different action, and `reached > 0` beside `tainted == 0` is the THIRD — "a sink
       ran and what reached it was the page's own strings", whose action is to go and look at the page. A run
       that inflates `reached` off non-sinks presents that reading for a document that reached no sink at all,
       whose action is the opposite one: build whatever the document died on.
       A CONCOLIC IS NOT DECLINED HERE, AND THAT IS STEP 2'S OTHER ARM RATHER THAN AN EXCEPTION TO THIS ONE.
       solve.h: "unknown external input is not a String, so `eval(location.hash)` takes step 2 and is DETECTED
       here" — and js_eval_program_source compiles a concolic's EXAMPLE when it carries one, so the arm this
       declines is exactly the arm nothing in the engine can ever run.
       THE SIBLING SINKS MUST NOT COPY THIS LINE, for the reason the verifying branch above already states:
       `location = { toString(){ return "javascript:X9()" } }` really does run ToString and really is a vector,
       so an object declined there would be a deleted finding rather than a declined non-sink. That is why the
       rule lives at this class's own entry and is ASSERTED at detect_sink, where all three converge. */
    if (!JS_IsString(arg) && !concolic_is(arg)) return;
    detect_sink(arg, SINK_EVAL);   /* record the source; breakout SEARCHED at verify */
}

/* …AND THE HOST'S OWN STRING-TO-CODE STEP — see solve.h for why HTML §8.7 Timers's substeps 9.8.2-9.8.8 are one
   of these and why they are not an ECMAScript eval.
   THE ANNOUNCEMENT IS FIRST AND IS UNCONDITIONAL, and that ordering is the whole content of this function. The
   detection arm needs the value that is NOT a program (unknown external input names no bytes, and that read is
   how the sink is found at all) and the candidate arm needs the value that IS one (a re-run substitutes a
   concrete breakout at the source, and these are the bytes solve_js.c scans for its locator and its §12 lexical
   state). A caller that announced only the arm it happened to be standing in had one of the two, and which one
   it kept decided which half of the ladder existed: keep the first and every candidate run is invisible, keep
   the second and the sink is never detected to have candidates at all. */
/* THE HOST SEAM'S COVERAGE — see solve_eval_sink_announced. It is a claim about ADJACENCY, exactly as the
   engine's per-compile latch is: it means "the program the caller is holding right now came out of the call
   below", so nothing that turns a string into a program may stand between the two. */
static int g_host_sink_announced;

int solve_eval_sink_announced(void) {
    int a = g_host_sink_announced;

    g_host_sink_announced = 0;
    return a;
}

JSValue solve_eval_sink_source(JSContext *ctx, JSValueConst handler) {
    JSValue text = JS_UNINITIALIZED;

    solve_eval_sink(ctx, handler);
    if (JS_IsString(handler)) {
        text = JS_DupValue(ctx, handler);
    } else if (concolic_is(handler)) {
        /* concolic.h: `concolic_example` answers JS_UNDEFINED when the operand carries no example yet, and an
           example of any other type is a value nobody holds program text for — which is §19.2.1.1 PerformEval
           step 2's arm exactly as a number written in the source position would be. Asked rather than assumed
           absent, because §solver's example is what makes `setTimeout(cfg.body)` over a LOADED config a program
           this engine runs instead of a read it shrugs at, and the engine already answers `eval` that way. */
        JSValue ex = concolic_example(ctx, handler);

        if (JS_IsString(ex)) text = ex;
        else                 JS_FreeValue(ctx, ex);
    }
    DCHECK(JS_IsUninitialized(text) || JS_IsString(text),
           "the host's string-to-code step answered with something that is not program TEXT and is not the "
           "absence of it — its caller compiles whatever comes back, so a third answer here is a value handed "
           "to a compiler that has no bytes to read");
    /* RAISED ONLY WHERE THERE IS A PROGRAM, because that is the state the consumer's assert is about: an arm
       that names no bytes compiles nothing, so there is nothing downstream for it to cover. */
    g_host_sink_announced = !JS_IsUninitialized(text);
    return text;
}

/* HTML firing oracle: re-parse the sink output with the REAL Lexbor parser and FIRE the auto-firing event
   handlers (svg/body onload, img/script onerror) by eval'ing their JS — X9 fires iff a breakout placed
   executable JS in an auto-firing position. innerHTML does NOT run <script>, so those never fire (correct). */
/* THE TREE'S DEPTH IS THE CANDIDATE'S DATA — a breakout that nests `<div>` a million times is exactly the kind
   of input this walk exists to run — so descending by C frame made the oracle's own depth attacker-controlled.
   Lexbor's nodes carry `parent`, so a pre-order traversal needs no stack and no allocation at all, which is
   what `node_next_in` is and why this walk uses it rather than descending. */
/* …AND IT ANSWERS §@S's CONTEXT-ESCAPED RUNG ON THE WAY, because the walk is already standing exactly where
   the question is decided. HTML §8.1.1 Introduction lists the mechanisms that "cause author-provided
   executable code to run" and "event handler content attributes" is one of them, so a handler attribute whose
   VALUE carries the marker is the markup class's definition of an executable position — the breakout left the
   §13.2.5 state it was written into and reached one. Returns whether any did.
   WHICH IS NOT THE SAME AS `fires`, and the two must not be read as one. `fires` counts every auto-firing
   handler in the parse, INCLUDING the page's own markup surrounding the injection point; the escape counts
   only handlers the breakout's own bytes are in. A page whose innerHTML template already contains an
   `<img onerror=…>` raises `fires` for a candidate that escaped nothing, which is precisely the reading the
   parked-search card used to state as a fact about the payload. */
/* THE WALK IS `node`'S OWN SUBTREE, through the engine's one pre-order successor. It used to carry its own
   advance, bounded by `node->parent` and testing that bound only on the CLIMB — which admits `node`'s following
   siblings and is the shape that, elsewhere, walked out of an inserted subtree and ran DOM §4.2.3 "Mutation
   algorithms"' post-connection steps over the rest of the document. Harmless here only because the one caller
   passes a document element, whose siblings are comments; stated as the subtree it always meant. */
static int html_fire_walk(Cand *e, lxb_dom_node_t *node) {
    lxb_dom_node_t *n;
    int at_exec = 0;

    for (n = node; n; n = node_next_in(n, node)) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            static const char *H[] = { "onload", "onerror", NULL };   /* AUTO-firing only (onmouseover needs interaction) */
            for (int h = 0; H[h]; h++) {
                size_t vl = 0;
                const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)H[h], strlen(H[h]), &vl);
                /* APPEND: an event handler fires from a TASK, so it takes the tail like every other task. */
                if (v && vl) {
                    /* THE ATTRIBUTE VALUE IS NOT NUL-TERMINATED — it is a (pointer, length) out of the DOM —
                       so the marker is searched inside its own bounds. A `strstr` here would read whatever
                       follows the value in Lexbor's own storage. */
                    for (size_t k = 0; k + 2 <= vl; k++)
                        if (v[k] == 'X' && v[k + 1] == '9') { at_exec = 1; break; }
                    e->fires++;
                    fire_js((const char *)v, vl, DYN_POS_APPEND);
                }
            }
        }
    }
    return at_exec;
}
/* AN ORACLE MAY NOT ANSWER "NO" BECAUSE IT COULD NOT ASK. All three of these were swallowed conditions, and
   what each of them swallowed is the same thing: a failure to build the parse silently becomes "this breakout
   did not fire", which is a FALSE NEGATIVE in the half of the tool that must never produce one — §@S is
   explicit that absence of a PoC is never a safe verdict, and an absence manufactured by an allocation is that
   verdict arrived at by accident. solve_html.c CHECKs this identical allocation two functions away, in the
   PROBE path, which is the weaker of the two places to care about it.
   THE FIRST TWO ARE `CHECK` AND THE THIRD IS A `DCHECK`, because they are different claims. A document that
   could not be allocated and a parse that returned anything but OK are both the physical floor — HTML
   §13.2's tokenizer and tree construction are error-RECOVERING and define no input they reject, so a non-OK
   status is memory and nothing else — and a security verdict quietly downgraded in a release build is worse
   than aborting. A parsed document with no document element is the parser's own invariant (§13.2.6 inserts
   html/head/body for every input, including the empty one), so it asserts against this engine's own logic. */
static void html_fire(Cand *e, const char *html) {
    lxb_html_document_t *doc = dom_document_create();
    lxb_dom_element_t *root;
    lxb_status_t st;

    CHECK(doc != NULL, "solve: OOM creating the document an @S markup breakout is fired in — without it the "
                       "oracle reports that the breakout did not fire, which is a false negative in the "
                       "security half rather than a missing measurement");
    /* THE PARSE IS RUN ON ITS OWN LINE, never inside the assert's condition: this one is a CHECK and would
       survive release, but a later reader converting it to a DCHECK would delete the whole parse with it. */
    st = html_parse_document(doc, DOM_PARSE_ROOT_PRIVATE, HTML_SCRIPTING_DISABLED, (const lxb_char_t *)html, strlen(html));
    CHECK(st == LXB_STATUS_OK,
          "the parse of an @S markup breakout did not complete — HTML §13.2 tree construction is "
          "error-recovering and rejects no input, so this is the allocation floor, and answering `did not "
          "fire` past it downgrades a real exploit to no finding");
    root = lxb_dom_document_element(&doc->dom_document);
    DCHECK(root != NULL, "a completed HTML parse produced no document element — §13.2.6 inserts html, head and "
                         "body for every input including the empty one, so the tree this oracle is about to "
                         "walk was built by something that is not the parser");
    if (html_fire_walk(e, lxb_dom_interface_node(root))) escape_reached(e);
    dom_document_destroy(doc);
}

/* URL firing oracle: navigating to a `javascript:` URL executes its JS. So the "fire" is: if the URL scheme is
   javascript:, eval the part after the colon — X9 fires iff the breakout made the URL a javascript: one. */
static void url_fire(Cand *e, JSContext *ctx, const char *url) {
    while (*url == ' ' || *url == '\t' || *url == '\n') url++;   /* leading whitespace is ignored by the URL parser */
    if (!strncasecmp(url, "javascript:", 11)) {
        const char *js = url + 11;
        /* APPEND: HTML §7.4.2.2 "Beginning navigation" queues a global task on the navigation and traversal
           task source to navigate to a javascript: URL, so §7.4.2.3.2's evaluation is a TASK — the same
           position engine_queue_javascript_url gives the real one. */
        /* §@S's CONTEXT-ESCAPED RUNG for the one class that has a single context. There is nothing to derive
           here — navigating executes the `javascript:` scheme and nothing else does — so the escape IS this
           test: the address the page built out of the attacker's bytes survived as a `javascript:` URL. It is
           still not the FIRE: the evaluation §7.4.2.3.2 "The javascript: URL special case" performs is queued
           as a task below, and the marker has to actually call inside it. */
        escape_reached(e);
        e->fires++;
        fire_js(js, strlen(js), DYN_POS_APPEND);
    }
}
/* location = arg (or el.href = arg): a URL-context sink. */
void solve_url_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {
        Cand *e;
        const char *url;
        if (concolic_is(arg)) return;
        /* AND THE HALF THAT TEST CANNOT SAY — see solve_eval_sink for the whole of it. It matters most HERE:
           this class has no context probe, so the marker alone identifies its bytes, and a page that ships a
           minified `X9` in a URL it builds itself would have raised the arrival rung AND called url_fire on
           its own address. */
        if (!concolic_candidate_delivered()) return;
        /* CONVERTED BEFORE THE CLASS PARTITION — see solve_eval_sink for why the filter rung is asked first. */
        if (!(url = JS_ToCString(ctx, arg))) return;
        filter_survived(url);
        if (!(e = candidate_search(SINK_URL))) { JS_FreeCString(ctx, url); return; }   /* another class's search owns this write */
        /* THE SAME PARTITION THE OTHER TWO CLASSES MAKE, and this sink was missing it. Every non-concolic URL
           the page wrote during a candidate run went to url_fire — including the page's OWN `javascript:`
           hrefs, whose JS was then queued as a program once per candidate. This class has no context probe, so
           the marker alone identifies its bytes: `CANDS_URL`'s vectors are the only strings this search ever
           injects and both carry X9. */
        if (strstr(url, "X9")) { breakout_arrived(e); url_fire(e, ctx, url); }
        JS_FreeCString(ctx, url);
        return;
    }
    detect_sink(arg, SINK_URL);
}

/* innerHTML = arg: an HTML-context sink. Detection records the source; the candidate run re-parses the injected
   HTML and fires its handlers. */
void solve_html_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {
        Cand *e;
        const char *html;
        if (concolic_is(arg)) return;   /* injection didn't reach this write */
        /* AND THE HALF THAT TEST CANNOT SAY — see solve_eval_sink. This is the sink whose own comment below
           records that "the value is not concolic" is true of every literal the page writes, which is the
           observation this line completes: it is also true of every literal the page writes, and until now
           each of those literals was measured against this candidate's payload. */
        if (!concolic_candidate_delivered()) return;
        /* CONVERTED BEFORE THE CLASS PARTITION — see solve_eval_sink for why the filter rung is asked first. */
        if (!(html = JS_ToCString(ctx, arg))) return;
        filter_survived(html);
        if (!(e = candidate_search(SINK_HTML))) { JS_FreeCString(ctx, html); return; }   /* another class's search owns this write */
        {
            /* TWO CANDIDATE KINDS REACH THIS WRITE, and each is told apart by the bytes IT injects — which the
               other cannot contain. The CONTEXT PROBE carries the inert locator and no X9 (that is what makes
               it inert); a derived BREAKOUT carries X9 and no locator (it is built from the tokenizer state,
               not from the probe's own token). So this is not a heuristic ordering, it is a partition.
               ONLY THE MARKER CAN FIRE, so a string without it cannot — and this too is EXACT rather than a
               heuristic: html_fire's only path to a report is an auto-firing handler whose code calls X9, and a
               parse cannot invent the marker out of bytes that do not contain it.
               It matters because "the injection did not reach this write" was being tested by "the value is not
               concolic", which is also true of every literal the page writes. So each candidate flow built a
               whole document and parsed EVERY innerHTML in the page — the fixture's own markup, once per
               candidate — and the cost is the page's markup times the number of breakouts tried. */
            if (strstr(html, SOLVE_HTML_LOCATOR)) { learn_witness(e, html); derive_from_witness(e); }
            else if (strstr(html, "X9"))          { breakout_arrived(e); html_fire(e, html); }
            JS_FreeCString(ctx, html);
        }
        return;
    }
    detect_sink(arg, SINK_HTML);
}

/* Fire-verify every pending source: SEARCH the candidate breakouts — inject each at the source, re-run the
   REAL program as a flow, and the FIRST that makes X9 fire is the replay-verified PoC (re-execution is the
   oracle, so no static context detection is needed). The re-run is a FLOW on the one frontier,
   the same path the scheduler uses — there is no separate boot re-runner. */
/* SEED the candidate flows: one per (detected sink, breakout), each an ordinary member of the ONE frontier.
   The scheduler runs them preemptibly and parkably like every other flow, which is what §solver requires — a
   driver that runs a candidate start-to-finish cannot park an unbounded loop inside it. */
/* Seed a candidate flow per (sink, breakout) for every sink NOT YET SEEDED, and answer how many were added.
   Idempotent by construction, so the scheduler can ask again every time the frontier drains — which is what a
   sink found by code that only loaded after the first drain needs. */
/* THE ROOT OF THE SEARCH A LIVE CANDIDATE BELONGS TO — the park's read half, asked by cold.c at the moment it
   writes that candidate's recipe.
   IT IS A FACT ABOUT THE SEARCH AND NOT ABOUT THE FLOW, which is why the flow does not carry one. A root is
   inherited unchanged through every derivation (see cand_learn_root), so the N candidates of one sink have
   one root between them and holding a copy on each Flow would be N owned strings that exist only to be
   asserted equal — plus a dup obligation at every clone, park and free site, which is exactly the shape
   §Architecture warns produces a field somebody forgets. The park DOCUMENT still writes one copy per record,
   and that is not the same duplication: a record is rebuilt on its own, by a session that has nothing else,
   so it has to be whole.
   THE SEARCH IS ALWAYS THERE TO ASK. A candidate flow exists only because detection opened its search
   (solve_flow_begin asserts the same thing on every switch-in) and a cold-resumed one re-registers before it
   runs an opcode, so an absent entry is a search dropped under a live flow rather than a question this file
   cannot answer. */
const char *solve_candidate_root(const char *src, const char *sink_name) {
    Cand *e;

    DCHECK(src && *src && sink_name && *sink_name,
           "a candidate flow was asked for its delivery root naming no source or no sink class — the pair IS "
           "the search's key, so either one missing asks about no search at all");
    e = search_of(src, sink_class_of_name(sink_name));
    DCHECK(e != NULL,
           "a candidate flow is being parked for a sink search this session has no entry for — the candidate "
           "exists only because detection opened one, so an absent entry means the search was dropped while "
           "one of its flows was still live, and the recipe about to be written names a search nothing reopens");
    DCHECK(e->root != NULL,
           "the search a candidate is being parked out of never learned how its bytes arrive — this is the "
           "last moment the fact exists in this process, and a record written without it resumes into a "
           "session that reports a fire-verified PoC as one no navigation reproduces");
    return e->root;
}

/* A PARKED CANDIDATE COMING BACK — see solve.h for why the re-binding, the bookkeeping and the REFUSAL are ONE
   call. */
const char *solve_resume_candidate(const char *src, const char *root, const char *sink_name,
                                   const char *payload) {
    int i, cls = -1;

    /* THE ROOT IS PART OF THE IDENTITY THAT CROSSES, and it was the part that did not. A resumed candidate
       opens its search here rather than at a detection — a verifying flow does not detect — so with no root
       in the record the entry stood at NULL and every emit of it hit emit_delivery's assert: in dev the whole
       report aborted, and in release the envelope rendered the silence that MEANS "no component carries these
       bytes" over a payload whose delivery the ended session knew exactly. */
    DCHECK(src && *src && root && *root && sink_name && *sink_name && payload && *payload,
           "a parked @S candidate was rebuilt without a source, without a delivery root, without a sink class "
           "or without its payload — its identity IS the substitution it carries and the route those bytes "
           "take to the victim, so any one missing makes it an exploration flow wearing a payload, or a "
           "payload nothing delivers. This asserts the record's SHAPE and not its content: the bytes are ones "
           "an earlier session of this engine wrote (cold.c's park_rec_cand asserts the same non-emptiness on "
           "the write side), so an empty field here is a residue this grammar did not produce, while WHAT the "
           "payload says is a stranger's business and is refused below rather than asserted");
    for (i = 0; i < SINK_CLASS_N; i++)
        if (!strcmp(SINKS[i].name, sink_name)) { cls = i; break; }
    if (cls < 0) {
        DFAIL("a parked @S candidate named a sink class this build's table does not have — the class crosses "
              "the tier by NAME exactly so it survives a pointer that cannot, so a name nothing matches is a "
              "residue from a build whose sink classes this one has dropped. Add the class back or drop the "
              "record; resuming it as an ordinary flow would report a search that never ran");
        return NULL;
    }
    /* PENDING, then the count on the entry the same call handed back. NOT `add_pending`: that OPENS a search
       (the class's written-down vectors, or its context probe), and this sink's search is already open — the
       flow being rebuilt is one of its candidates. Opening it again would seed the whole search a second time,
       which is precisely what the park's write-once assert exists to prevent, arriving through the other door.
       It dedups, so a session that resumes five candidates for one sink registers it once and raises `tried`
       once per candidate it ACCEPTS — exactly the number of candidate runs that sink's search has had, and
       exactly what the parked-search entry reports. Five records are not therefore five runs: the refusal
       below can withdraw any of them, and `tried` counts runs rather than records precisely so that the two
       stay distinguishable when they differ. */
    {
        int created = 0;
        Cand *e = sink_search(src, cls, &created);
        /* AND THE ROOT GOES ON BEFORE THE ENTRY IS VISIBLE TO ANY READER, which is what makes this the second
           of the search's two doors rather than a hole beside the first. On the fifth resumed candidate of one
           sink this is cand_learn_root's equality assert over what the fourth wrote, which is the only thing
           that can say a park document's records still agree with each other. */
        cand_learn_root(e, root);
        /* …AND THE SAME DOOR THAT LEARNS THE ROOT IS WHERE THE PAYLOAD IS REFUSED, because learning the root
           IS what makes the refusal answerable: cand_learn_root seeds this table's carrier half from the
           declaration, so the line above is the first instant in this session at which these bytes can be
           asked about at all. Asked with solve_delivered_ok — the SAME predicate solve_seed_candidates asks of
           a freshly derived escape — so the two doors onto a candidate flow share one refusal rather than
           spelling two that can drift apart.
           WHAT FALSIFIES THE RECORD'S PREMISE, stated plainly because §NO BOUNDS requires it of every
           narrowing: a parked candidate asserts that these bytes reach this sink as themselves, and the root's
           carrier declaration says one of them never enters the page's program IN ANY FORM — RFC 6265 §4.1.1's
           cookie-octet excludes it and the plant does not percent-encode it either. That is a positive
           contradiction and not a fact merely consistent with the premise: no page-side transform recovers a
           byte that was never carried, so no run can widen this half of the table and no later observation can
           reinstate the record. It is the same pruning solve_seed_candidates performs on a contradicted
           spelling, and it is not a cap — there is no count here, no age, no retry limit and no seen-set, and
           the WORLD the record named is not deleted with it: the recipe outlives the bytes. The flow keeps its
           decision segment and comes back as an ORDINARY exploration flow (cold.c drops the four candidate
           fields), so it replays the path that reached this sink, DETECTS there, and re-opens the search — and
           the search then derives its escapes against the table this very call has already narrowed. The
           contradicted spelling is what is refused; the search that would construct a deliverable one is
           re-derived rather than lost.
           IT CANNOT WITHDRAW AN INSTRUMENT, WHICH IS THE ONE THING THE SEED DOOR NEEDS ITS `kind` FOR AND THIS
           ONE HAS NO WAY TO ASK. A resumed payload has no row in `pl` (cand_kind_of returns 0 for it by
           construction — see its note), so probe-versus-escape is not a question this door can answer; it does
           not have to be. Both instruments are printable US-ASCII BY CONSTRUCTION — the context probe is
           ASCII alphanumeric so it cannot change the parse it reads, and the delivery probe is a token plus a
           decimal digit plus a DECLARED byte (bytes_probe) — while the refusal covers exactly the bytes
           OUTSIDE printable US-ASCII (carrier_shared_byte), and cand_learn_root's own two-sided assert holds
           the declared and refused sets disjoint. So no probe this file builds can be refused here, and the
           seed door's `kind == CAND_PROBE` exemption has nothing to guard against on this path.
           NOTHING IS COUNTED FOR A WITHDRAWAL. `tried` and `resumed` are counts of candidate RUNS and this
           record produces none, so raising either would report a run the search never had; `g_cands_seeded` is
           the cost of a document re-run and no re-run is spent. What IS raised is the withdrawal itself, which
           is the third state the pair above cannot express and the evidence add_pending reads. */
        if (!solve_delivered_ok(&e->deliv, payload)) {
            e->resumed_withdrawn++;
            return NULL;
        }
        e->tried++;
        /* …AND WHICH OF THOSE RUNS THIS IS, because `tried` alone cannot say. A reader holding `tried:6` beside
           an empty `payloads` list is looking at either a cross-session search whose every run came back from
           a park document, or a producer that dropped the payload field — solve.h's arithmetic names the two
           terms and only one of them was ever a number. This is the other one. */
        e->resumed++;
    }
    /* IT COSTS WHAT A FRESH ONE COSTS, so it counts as one. This number is what says whether a run got slower
       because there were more searches or because each search grew, and a resumed candidate re-runs the whole
       page exactly as a newly-seeded one does. */
    g_cands_seeded++;
    return SINKS[cls].name;
}

/* THE CURSOR IS WHAT MAKES A DERIVED BREAKOUT REACHABLE. The old test was `if (tried) continue` — a sink was
   seeded once, out of a list its class knew before the page ever ran, and never looked at again. A derived
   context is not known then: the probe flow has to RUN first, and it appends what it constructs to a search
   that has already been seeded once. Taking everything past the cursor says both things at once — nothing is
   seeded twice, and nothing appended later is missed. */
int solve_seed_candidates(JSContext *ctx) {
    int added = 0;
    for (int i = 0; i < g_pending_n; i++) {
        Cand *e = &g_pending[i];
        /* THE SAME QUESTION THE DERIVATION ASKS, ASKED AGAIN HERE BECAUSE THE TWO ARE NOT THE SAME MOMENT. A
           breakout appended by one candidate flow is seeded at the NEXT drain, and between those two points a
           different candidate of the same search can fire — which closes the search and releases the path the
           install below reads. Declining the append alone would leave that window open. */
        if (!search_seeds(e)) continue;
        for (; e->seeded < e->npl; e->seeded++) {
            Flow *f;
            /* WITHDRAWN — the search's OWN measurement has since contradicted this spelling, so it does not
               become a flow. queue_derived asks solve_delivered_ok at PUSH time and the table narrows
               afterwards (observe_delivery, on a token the delivery probe put in front of the byte), so a
               breakout constructed while everything still delivered stays in `pl` while the constraint that
               permitted it is gone. Left alone it is seeded a WHOLE DOCUMENT RE-RUN whose only possible
               outcome is the candidate arriving transformed — the exact cost queue_derived's own assert says
               it exists to prevent, arriving one moment later through a door that never re-asked.
               IT IS PRUNING A CONTRADICTED ARM, WHICH §Solver-half LICENSES, AND IT IS NOT A BOUND. There is
               no count, no age, no retry limit and no seen-set here: the ONE question is whether something
               POSITIVE contradicts these bytes arriving — this search's own delivery run, or the refusal its
               root's carrier declares (cand_learn_root), which are two facts of different kinds and one
               instruction here. The table narrows only on evidence (a token that never showed up says nothing
               about its byte, so uncertainty keeps the arm) and it never widens, so a withdrawal is permanent
               and the cursor stays a cursor — nothing is re-examined, nothing is seeded twice, and a spelling
               the tightened table still permits is seeded exactly as before.
               THE ENTRY STAYS IN `pl`, AND THAT IS THE REPORT'S HALF OF THE SAME FACT. A search that
               CONSTRUCTED an escape and withdrew it is not a search that constructed none — `probes ==
               payloads` means the second, and compacting the list would say it. `withdrawn` (solve_json_array)
               is what tells the two apart per entry, and it is also what keeps `tried` and `payloads`
               readable together now that they legitimately differ.
               PROBES ARE NOT ESCAPES AND ARE NEVER WITHDRAWN. solve_filter.c's own header says the question is
               asked of "a constructed escape"; the DELIVERY probe is built OUT OF the very bytes in question,
               so a table it narrowed would contradict the instrument that measured it, and the CONTEXT probe
               is ASCII alphanumeric precisely so it cannot change the parse it reads. Both are measurements,
               not attacks, and the entry's own KIND is where that line is — asked of the candidate about to
               become a flow rather than of its position in the list. */
            if (e->pl[e->seeded].kind == CAND_ESCAPE &&
                !solve_delivered_ok(&e->deliv, e->pl[e->seeded].bytes)) continue;
            f = flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* a candidate session runs from the baseline */
            f->cand_src     = strdup(e->src);
            f->cand_payload = strdup(e->pl[e->seeded].bytes);
            f->cand_sink    = sink_name(e->sink);
            CHECK(f->cand_src && f->cand_payload, "solve: OOM seeding a candidate flow");
            /* THE OTHER SIDE OF THE WITHDRAWAL, ASSERTED WHERE THE COST IS TAKEN RATHER THAN WHERE THE
               DECISION IS MADE. This line is the moment a payload becomes a document re-run, so it is the one
               place at which "a contradicted candidate is still queued" stops being a list state and starts
               costing a traversal — and the push-time check being the ONLY check is exactly what let that
               happen once. A route into flow creation that does not pass the skip above crashes here instead
               of quietly spending the run and the rung. */
            DCHECK(e->pl[e->seeded].kind == CAND_PROBE || solve_delivered_ok(&e->deliv, f->cand_payload),
                   "a candidate flow was created for a payload this search has OBSERVED cannot reach its sink — "
                   "the delivery table narrows after a breakout is queued, so deliverability asked once at the "
                   "derivation is a constraint that has expired by the time the flow is made, and this run can "
                   "only end with the candidate arriving transformed at its own sink");
            /* …AND ON THE PATH THE DETECTION ALREADY PROVED REACHES THIS SINK. The flow still re-runs the
               document from the baseline — the payload enters at the source read and there is no earlier point
               to start from — but it CONSUMES the recorded arms at each branch it re-reaches instead of forking
               over them, so it walks the one arm that arrives rather than searching a tree as deep as the
               document's gate sequence for it. `decide_blob_new` takes its own reference on the frozen segment
               and replays at cursor 0, forking normally the moment the cursor runs past what the detecting flow
               knew — which is where this candidate's own exploration begins.
               UNCONDITIONAL, AND THE `if (e->reinject)` THAT STOOD HERE IS DELETED WITH ITS REASON. It said a
               search with no re-injection point "is not a fallback, it is the other algorithm: a single-context
               class has no probe, so there is no path to replay and its written-down vectors are seeded at
               DETECTION, when this problem does not arise" — and that last clause is the part measurement
               contradicts. A vector seeded at detection re-forks the document's whole gate tree exactly as a
               probe does (see the field's own note for the numbers); what a single-context class lacks is a
               DERIVATION, never a path. add_pending now takes the path for every search at the one moment a
               flow stands at the sink, so there is nothing left to select between and a NULL here is a search
               opened by a door this file does not have. */
            {
                DCHECK(e->reinject != NULL,
                       "a candidate is being seeded for a search that holds no re-injection point — every "
                       "search is opened by add_pending, which freezes the detecting flow's path before it "
                       "pushes the first breakout, so a payload list with no path behind it belongs to an "
                       "entry that acquired breakouts without ever being detected");
                DCHECK(f->dec_blob == NULL && f->pin_blob == NULL && !f->started,
                       "a freshly added flow already carries decision state — the re-injection install below "
                       "would overwrite it and drop that segment's reference, and the flow would replay a path "
                       "that is not the one it was given");
                /* THE TRIPLE, AND IT IS ONE THING RATHER THAN THREE ASSIGNMENTS. A flow standing on a
                   RECORDED path is `started` — flow_switch_in routes on exactly that bit, and a flow that has
                   never run takes decide_enter, which replays from NOTHING and never looks at `dec_blob`.
                   Setting the blob alone therefore does not merely fail to work, it fails SILENTLY IN BOTH
                   DIRECTIONS: the path is ignored, and the pointer is still live at the flow's first suspend,
                   where engine.c does `f->dec_blob = decide_suspend()` and overwrites it — leaking the blob
                   AND its reference on the frozen segment, which keeps the whole prefix under it alive. That
                   is the exact ceiling this field's own comment says is released here, defeated by the two
                   assignments it did not make. Measured: two derived searches whose breakouts read
                   `survivedBy:[16,0]` after the re-injection landed, because it had never once been read.
                   THE EMPTY PIN BLOB IS THE THIRD MEMBER AND NOT A COURTESY. flow_switch_in's resume branch
                   calls concolic_pins_resume beside decide_resume, so a flow marked started with no pin blob
                   hands it NULL; cold.c pairs the two for this reason and says so ("the empty pin blob is what
                   makes the second half of that true"). A replaying flow re-derives every pin from the gates
                   it replays, so EMPTY is the correct content and not a placeholder.
                   cold.c's 'f' and 'c' arms are the other installer of this triple, and they are the reason it
                   is known to work: a cold-resumed candidate IS a candidate flow standing on a path it did not
                   itself run. */
                f->started  = 1;
                f->dec_blob = decide_blob_new((void *)decide_blob_seg(e->reinject));
                f->pin_blob = concolic_pins_blob_empty();
            }
            added++;
            g_cands_seeded++;
            e->tried++;
        }
        DCHECK(e->npl > 0 || e->tried > 0,
               "a detected sink has neither a candidate to run nor a run behind it — its class opened no "
               "search for it, so it would be reported as parked forever with nothing ever tried");
    }
    return added;
}

/* THE SUBSTITUTION MIRRORS THE RUNNING FLOW — it is not a bracket someone opens and closes.
   Written as a bracket it was WRONG, and silently: the entry returned early for a flow with no candidate, so
   switching from a candidate flow to an ordinary one left the previous candidate's payload installed and
   endpoint recording suppressed. The exploring flow then read the attacker's concrete string where its concolic
   source belonged — so it stopped forking at the gates that value feeds, its sinks stopped being detected (a
   concrete value is not a concolic one), and every endpoint it learned was dropped. The comment that used to
   sit here asserted the opposite ("an ordinary flow scheduled in between is unaffected"), which is exactly the
   sort of claim that survives because nothing tests it.
   The scheduler calls this on EVERY switch-in, so the fix is for it to install the incoming flow's state
   unconditionally — a flow with no candidate installs "no candidate", which is the clearing that was missing.
   There is then no close to forget, and no ordering between two calls to get wrong. */
/* THIS SEARCH'S BYTES JUST ENTERED THE PAGE'S OWN PROGRAM — the REPORT's bottom rung, written from the one
   site that can state it. §@S(i) requires every rung to have an observation site strictly before the thing it
   is a distance to, and every other number on a parked entry reports AT a sink; this one reports at the SOURCE
   READ, which is the only observation available on the runway that is not a claim about where the bytes have
   got to (that would need a taint tracker, which §Re-execution bans).
   IT IS THE REPORT'S COPY OF THE EVENT flow_observe_rung RECORDS AS FLOW_RUNG_DELIVERED, and the two are
   deliberately separate quantities at separate accounting units — §@S(ii). The flow's rung is the COMPARATOR:
   monotone, per flow, re-earned across a park, read at the pick, and it orders live candidates. This is a
   COUNT on the SEARCH: it never orders anything, it survives the flows that produced it, and it is the only
   one of the two a reader ever sees. Neither can be derived from the other — a search whose every candidate
   has been paged out still reports what its runs did, and a flow's rung says nothing about how many runs there
   were.
   NOT A CREDIT AND NOT A CROSSING. Nothing here calls flow_credit_emit, latches a rung or moves the frontier
   generation, so the WFQ cannot see this write at all. Recorded as a ledger quantity it would be a 0->1
   crossing paid to the first candidate of the search to reach its own source read and to none of the rest,
   which is §@S(ii)'s defect exactly — the second delivery teaches the search nothing the first did not.
   THE TRIPLE IS `CHECK`ED AND NOT ONLY `DCHECK`ED because both halves of it are dereferenced immediately:
   solve.c states the same rule about filter_survived's own three, and a flow carrying half a substitution is
   an assembly that went round the seeder and the cold tier's rebuild rather than a recoverable state. */
void solve_observe_substitution(Flow *f) {
    Cand *e;

    CHECK(f != NULL && f->cand_src != NULL && f->cand_sink != NULL,
          "solve: an @S substitution was performed for a flow carrying no source or no sink class — a search "
          "is the pair, and both are dereferenced immediately below, so a flow holding half of one is a "
          "candidate assembled outside solve_seed_candidates and solve_resume_candidate");
    e = search_of(f->cand_src, sink_class_of_name(f->cand_sink));
    CHECK(e != NULL,
          "solve: an @S substitution was performed for a search this session has no entry for — a candidate "
          "exists only because detection opened one and a cold-resumed one re-registers before it runs an "
          "opcode, so an absent entry is a search dropped under a live flow");
    /* …AND THE LINK BENEATH IT, WHICH IS THE ONE RUNG OF THE LADDER NOTHING ASKED FOR. The chain this file
       already asserts runs DOWNWARD from the top — breakout_arrived and filter_survived each demand
       `substituted > 0`, learn_witness demands `turns > 0` — so every link is guarded except the one between
       those two, and that is exactly the link a report reading `tried:N,turns:0,substituted:0` stands on. A
       substitution is performed by a candidate flow AT ITS OWN SOURCE READ, and a flow reaches a source read
       only while it HOLDS THE THREAD, which is the switch-in solve_flow_begin counts; so `substituted:D` over
       `turns:0` is not a search that ran unscheduled, it is `turns` counting something other than the flows
       that do this search's work — and the whole reading of `turns:0` as WFQ starvation rests on it not being
       that. Unasserted, the two states are one number, which is the tell §@S names.
       IT IS learn_witness's INVARIANT TWO RUNGS LOWER AND IS WRITTEN OUT RATHER THAN SHARED WITH IT: a DCHECK
       stamps the line it is written at, so one helper for both would report one line for two different events
       and its remedy would name an action with no object. */
    DCHECK(e->turns > 0,
           "an @S candidate performed its substitution for a search the scheduler reports it has never given "
           "a turn to — solve_flow_begin raises `turns` at every switch-in of a candidate flow, and a flow "
           "reaches its own source read only while it holds the thread, so `turns` is not counting the flows "
           "that do this search's work and `turns:0` would be read as WFQ starvation for a search that ran");
    e->substituted++;
}

/* THE RUNWAY READING, TAKEN AT THE TWO MOMENTS THIS FILE HAS AND NOT AT A THIRD IT WOULD HAVE TO INVENT.
   `Flow.cand_replay` moves inside dec_replay, arm by arm, in a component this file does not call and cannot
   be called back by; what solve.c holds are the two seams engine.c already routes through it — the switch-IN
   (solve_flow_begin, which is where `turns` is counted) and the FINISH (solve_flow_end). So the reading is
   taken there, and the ratchet is what makes two sparse samples add up to the search's furthest: a per-flow
   value that only ever rises, sampled at every switch-in of every candidate and once more when one ends.
   THE RESIDUAL IS NAMED BECAUSE IT IS REAL AND SMALL. A candidate that takes the thread, walks part of its
   runway and is then PARKED or is still live when the document is emitted contributes only what its last
   switch-in saw — engine.c has no solve-side switch-OUT seam, and inventing one to close this would be a
   second door into the candidate state for a report counter's benefit. It shows as a `runwayPerMille` that
   under-reads a search whose flows are long-lived and rarely switched; it can never over-read, because every
   value sampled is one this flow had already reached. The next diff that needs the tighter number takes the
   reading where flow.c already has it — flow_observe_replay's signature is `(Flow *, long consumed, long
   total)`, so the numerator and denominator the pair form is available at that site and at no other.
   READ THROUGH THE FIELD AND NOT THROUGH flow_distance, deliberately: flow_distance is the whole comparator
   (`(cand_replay + cand_surv + cand_rung) / FLOW_RUNGS_N`) and taking the runway out of it would be this file
   re-deriving another component's arithmetic — the thing an auditor is forbidden to do, one level down. */
static void observe_runway(Cand *e, const Flow *f) {
    int pm;

    DCHECK(f->cand_replay >= 0.0 && f->cand_replay <= 1.0,
           "an @S candidate's runway position is not a fraction of its own recorded path — flow.c's "
           "flow_observe_replay asserts [0,1] at every write and flow_observe_rung pins it to exactly 1.0 at "
           "the delivery, so a value outside that range is the ladder's bottom rung carrying something other "
           "than the fraction this record is about to publish as one");
    pm = (int)(f->cand_replay * 1000.0 + 0.5);
    if (pm > e->replay_pm) e->replay_pm = pm;
    /* AND THE PAIR, RATCHETED ON ITSELF. It cannot be derived from the line above and it cannot ride that
       line's condition: `pm` is the rounded value, so a best fraction of 3/8000 leaves it at 0, never exceeds
       a stored 0, and the sample that is the whole reason this pair exists is the one that would be dropped.
       CROSS-MULTIPLIED AND NOT DIVIDED, so the comparison is exact and no float is stored: with both
       denominators positive, `a/b > c/d` is `a*d > c*b`. `of == 0` is this search's word for "no reading yet"
       on the left and this flow's on the right, so each is tested rather than allowed to make a product of
       zero decide anything. */
    DCHECK(f->cand_replay_of >= 0 && f->cand_replay_arms >= 0 &&
           f->cand_replay_arms <= f->cand_replay_of,
           "an @S candidate's runway pair is not a position on its own path — flow_observe_replay writes the "
           "two together from one sample and asserts `consumed <= total` at the write, so a numerator past its "
           "denominator here is a pair assembled out of two different moments and the fraction it makes would "
           "be published as a replay that walked further than the path it was walking");
    DCHECK(f->cand_replay_of <= 0x3fffffff && e->replay_of <= 0x3fffffff &&
           f->cand_replay_arms <= 0x3fffffff && e->replay_arms <= 0x3fffffff,
           "a runway pair is large enough that the cross-multiplication below could overflow — the products "
           "are formed in long long and are safe to a billion each, so a value past that is not a decision "
           "vector's length but a field that has been written by something other than the one sample that "
           "owns it");
    if (f->cand_replay_of != 0 &&
        (e->replay_of == 0 ||
         (long long)f->cand_replay_arms * (long long)e->replay_of >
         (long long)e->replay_arms * (long long)f->cand_replay_of)) {
        e->replay_arms = f->cand_replay_arms;
        e->replay_of   = f->cand_replay_of;
    }
}

void solve_flow_begin(Flow *f) {
    concolic_set_candidate(f ? f->cand_src : NULL, f ? f->cand_payload : NULL);
    endpoint_suppress(f && f->cand_src ? 1 : 0);
    if (f && f->cand_src) {
        f->cand_verifying = 1;
        /* AND THE SEARCH IS GIVEN A TURN — counted HERE because this is the one point at which a candidate of
           it is about to execute, and because `reached:0` cannot otherwise be read. `tried` says candidates
           were SEEDED and `reached` says a breakout ARRIVED at the sink; with both of those a search reporting
           `tried:2,reached:0` is either a search whose flows the WFQ has never once given the thread to, or one
           whose flows have run and have not got as far as the sink. Those take opposite actions — the first is
           a scheduling question, the second a distance-through-the-document one — and the pair could not tell
           them apart. Measured on this fixture: the @S sinks sit 866 statements into a script whose START is
           where every lane appends, while --min's sit 3 statements in and arrive; which of the two explains it
           is exactly what this number decides.
           IT IS SWITCH-INS AND NOT DISTINCT FLOWS, which is what makes it a scheduling fact rather than a
           second copy of `tried`: a candidate preempted and resumed twenty times has been given twenty turns,
           and that is the thing being asked about. */
        Cand *e = search_of(f->cand_src, sink_class_of_name(f->cand_sink));
        DCHECK(e != NULL,
               "a candidate flow was switched in for a sink search this session has no entry for — a candidate "
               "exists only because detection opened one, and a cold-resumed one re-registers before it runs "
               "(solve_resume_candidate), so an absent entry means the search was dropped under a live flow");
        /* …AND THE LINK BENEATH THIS RUNG, for the reason solve_observe_substitution states about the one
           beneath IT. Both doors raise `tried` before the flow can ever be picked — solve_seed_candidates at
           the creation itself, solve_resume_candidate during the cold rebuild and before the flow runs an
           opcode — so a switch-in standing over `tried:0` is a THIRD door, and the card would report turns
           with no seeded candidate behind them. It is the pair `s-*-seeded` and `s-*-ran` are read as, stated
           where the second of them is written.
           A WITHDRAWN RECORD IS NOT A COUNTEREXAMPLE TO THAT, and the reason is worth stating because the two
           doors no longer raise `tried` unconditionally. solve_resume_candidate can refuse a parked candidate
           whose payload the root's carrier contradicts, and it raises `resumed_withdrawn` instead — but the
           same refusal makes cold.c drop `cand_src`, and `cand_src` is the whole of what the branch above
           tests, so a withdrawn record never reaches this line as a candidate at all. The claim here is about
           every flow that DOES reach it, and it is still exactly true of all of them. */
        DCHECK(!e || e->tried > 0,
               "an @S candidate flow was switched in for a search that reports no candidate seeded — both "
               "doors into a candidate raise `tried` before the flow is reachable by the pick, so a turn "
               "standing over `tried:0` is a candidate assembled outside them and the parked card is about "
               "to report turns with nothing behind them");
        if (e) e->turns++;
        /* AND THE RUNWAY THIS CANDIDATE HAD ALREADY WALKED WHEN IT LAST HELD THE THREAD — taken at the same
           moment `turns` is raised because they are the two halves of one reading: `turns` says the WFQ gave
           this search the thread and this says how far the thread got it. Sampled BEFORE the quantum rather
           than after it, which is what makes it a fact about runs that have finished rather than a promise
           about the one about to start. */
        if (e) observe_runway(e, f);
    }
}

/* FINISHING IS A DIFFERENT EVENT FROM SWITCHING OUT, AND IT RECORDS NOTHING. It used to be where a fired
   candidate became a finding, and that is deleted: the fire is recorded at the marker (js_x9), because that
   is where the proof happens and because a flow owes nobody completion. The globals are deliberately NOT
   cleared here: the next switch-in installs the next flow's state, and clearing in two places is how the
   asymmetry above got in.
   WHAT IS LEFT DEAD BY THAT AND IS NOT THIS FILE'S TO REMOVE: `Flow.cand_fired` (solver/flow.h) is now
   written by nothing, so engine.c's sibling copy of it and cold.c's deliberate drop of it are both statements
   about a field that no longer carries anything. They go with the call to this function, and this function
   with them. */
void solve_flow_end(Flow *f) {
    if (!f || !f->cand_src) return;
    /* THE LAST RUNWAY READING THIS CANDIDATE WILL EVER OFFER, and the one the switch-in sampling structurally
       cannot take: a flow that holds the thread from its final switch-in to its own end contributes nothing
       through solve_flow_begin, and that is exactly the candidate that walked furthest. Taken before
       `cand_verifying` is cleared so the two statements about this flow are made in one place. */
    Cand *e = search_of(f->cand_src, sink_class_of_name(f->cand_sink));
    DCHECK(e != NULL,
           "a candidate flow ended for a sink search this session has no entry for — a candidate exists only "
           "because detection opened one and a cold-resumed one re-registers before it runs an opcode, so an "
           "absent entry here is the search having been dropped under a flow that was still running it, and "
           "the runway this candidate walked is about to be lost with it");
    if (e) observe_runway(e, f);
    f->cand_verifying = 0;
}

/* ── @S JSON emit (C-native) ── the writer is core/json_buf.h's, which this file and endpoint.c each used to
   carry a private copy of. */
static int solved(int cls, const char *src) {
    for (int i = 0; i < g_sinks_n; i++)
        if (g_sinks[i].cls == cls && !strcmp(g_sinks[i].source, src)) return 1;
    return 0;
}

/* THE SOURCE'S DECLARED BROWSER DELIVERY, written identically into BOTH entry shapes because it is ONE
   declaration and the two entries need different halves of it: `sourceEncodes` is what a BREAKOUT had to
   survive (the parked entry's constraint), and the mechanism plus its address component are what a
   REPRODUCTION has to perform (§S(d)'s envelope). Splitting them across two writers is how one of them went
   missing before.
   AN UNDECLARED SOURCE WRITES NOTHING, and that silence is the fact that there IS no declaration — server-
   injected page state is written by the attacker directly, no component carries or transforms it, and a
   consumer must say exactly that rather than invent a vector. The vocabulary is the engine's: the delivery
   layer switches on these tokens and states its own inability to perform one, but it never decides which
   source uses which — the component that owns the source already did. */
/* IT IS ASKED WITH THE ROOT AND NOT WITH THE INJECTION IDENTITY, and the difference is a whole finding. The
   registry is an exact strcmp over the declared rows: `location.hash` matches and `{location.hash}.slice()` —
   what `location.hash.slice(1)` composes, and what a page that strips its own `#` therefore reports as — does
   not, so both halves of the declaration went missing for the derivations real code is written in. See the
   record above for what the popup then printed.
   AN ABSENT ROOT IS NOT AN UNDECLARED SOURCE. Undeclared is a FACT (server-injected page state is written by
   the attacker directly and no component carries it), and it is what silence here means; a record that never
   learned its root is a record this session cannot answer for, and rendering the two the same way is exactly
   the lie this change removes. The assert below is now an INVARIANT rather than a work item: both doors into
   g_pending state the root, so a NULL naming neither of them is a third door. */
/* WHICH OF THE DECLARED BYTES A RUN ACTUALLY SAW ARRIVE — the MEASURED half of the constraint, beside the
   DECLARED one. `sourceEncodes` says what the browser does on the way in; this says what the page's own code
   left of it, and the two are different facts whose difference IS the finding: equal sets mean the page decodes
   its own fragment and every §13.2.5 escape is on the table, an empty set beside a full declaration means the
   search's whole failure is the source's transform and no re-derivation can help.
   ANSWERS NULL WHERE THERE IS NOTHING TO SAY, and both silences are positive: a source that declares no
   percent-encode set has no byte in question, and a search whose delivery probe has not run has taken no
   measurement — which is why the permissive initial table is never emitted as one. */
static const char *cand_delivers(const Cand *e, char *buf, size_t n) {
    const char *enc = e->root ? concolic_source_encodes(e->root) : NULL;
    size_t o = 0, i;

    if (!e->deliv_seen || !enc || !*enc) return NULL;
    CHECK(strlen(enc) < n,
          "solve: a source's percent-encode set does not fit the buffer the measured half is written into — "
          "the measured set is a SUBSET of the declaration, so a declaration that does not fit means the "
          "report is about to state a truncated constraint as the whole of one");
    for (i = 0; enc[i]; i++) if (e->deliv.ok[(unsigned char)enc[i]]) buf[o++] = enc[i];
    buf[o] = 0;
    return buf;
}

static void emit_delivery(JsonBuf *b, const char *root, const char *delivers) {
    const char *enc, *kind = NULL;
    char prefix = 0;

    DCHECK(root != NULL,
           "an @S record is being emitted with no delivery ROOT. Exactly two paths open a search and both "
           "state one: detection reads it off the value (detect_sink asserts it for all three classes) and a "
           "cold resume takes it out of the 'c' record's own root field (cold.c's park_rec_cand writes it, the "
           "'c' arm of cold_resume reads it back, solve_resume_candidate learns it). So an absent root here is "
           "a THIRD way into g_pending, and what it costs is the difference between 'no component carries "
           "these bytes to the victim' and 'this session cannot say' — which the report renders identically");
    enc = concolic_source_encodes(root);

    if (enc) { json_buf_raw(b, ","); json_buf_key(b, "sourceEncodes"); json_buf_str(b, enc); }
    /* IMMEDIATELY AFTER THE DECLARATION IT IS A SUBSET OF, because the pair is one statement and a reader has
       to be able to hold the two against each other. */
    if (delivers) {
        DCHECK(enc != NULL,
               "a measured delivery set is being emitted for a source that declares no percent-encode set — "
               "the measured set is the subset of the DECLARED one a run saw arrive, so a measurement with no "
               "declaration behind it is a subset of nothing");
        json_buf_raw(b, ","); json_buf_key(b, "sourceDelivers"); json_buf_str(b, delivers);
    }
    if (concolic_source_delivery(root, &kind, &prefix) && kind) {
        json_buf_raw(b, ","); json_buf_key(b, "delivery"); json_buf_str(b, kind);
        if (prefix) {
            char p[2] = { prefix, 0 };
            json_buf_raw(b, ","); json_buf_key(b, "deliveryPrefix"); json_buf_str(b, p);
        }
    }
}

/* EVERY DETECTED SINK IS REPORTED — the ones with a fire-verified PoC, AND the ones whose search has not solved.
   Emitting only the solved ones made the report say nothing at all about a sink an attacker source demonstrably
   REACHES, and a reader cannot tell that silence apart from "no attacker input gets here" — which is the
   "safe"/"verified:false" verdict solve.h forbids, arrived at by omission instead of by claim. A sink without a
   PoC is a PARKED SEARCH: reached, searched this far, not broken out of YET.
   It carries the two facts that make it actionable rather than a shrug: how many candidate runs it has had, and
   the bytes the source's own component percent-encodes — the constraint every candidate had to survive. For
   `innerHTML` fed from `location.hash` that set contains `<`, which is why no HTML-context candidate can fire
   and why the same source's JS-context sink does; the entry states the constraint, it does not claim the sink
   is safe, because an app that percent-DECODES its fragment would break out with the same candidate. */
char *solve_json_array(JSContext *ctx) {
    JsonBuf b = { 0 };
    int n = 0;
    json_buf_raw(&b, "[");
    for (int i = 0; i < g_sinks_n; i++) {
        const SinkClass *sc = sink_class(g_sinks[i].cls);
        const PolicyContainer *pc = document_policy(ctx);

        if (n++) json_buf_raw(&b, ",");
        json_buf_raw(&b, "{"); json_buf_key(&b, "sink"); json_buf_str(&b, sc->name);
        json_buf_raw(&b, ","); json_buf_key(&b, "source"); json_buf_str(&b, g_sinks[i].source);
        json_buf_raw(&b, ","); json_buf_key(&b, "poc"); json_buf_str(&b, g_sinks[i].poc);
        /* §S(d): EVERY PoC CARRIES ITS REPRODUCTION ENVELOPE. What makes it RUN is the first thing a reader
           needs and the last thing this record carried — the vector was decided one line below, for the CSP
           question, and thrown away. It comes off the sink's own fire oracle (see the table). */
        json_buf_raw(&b, ","); json_buf_key(&b, "firesOn"); json_buf_str(&b, sc->fires_on);
        /* §S: A FIRING BREAKOUT IN THE MODEL IS NOT YET A WORKING EXPLOIT. The PoC has to run under the page's
           ACTUAL policy, and an inline `onerror` is dead under `script-src 'self'`. Reporting a bare XSS there
           is a false positive a reader cannot tell from a real one; reporting nothing would hide a sink that IS
           real. So the finding stays, and it CARRIES what blocks it — "sink REAL, CSP blocks", which is the
           standard's own distinction and the only one that survives being read by someone else. */
        /* THE PoC IS THE `source` §6.7.3.3 IS ASKED ABOUT, and the ELEMENT IS NULL because this breakout has
           been inserted nowhere — the same shape §4.2.4 uses when it runs the inline check "upon null" for a
           javascript: navigation. That NULL is what keeps the nonce arm out of the answer, which is correct:
           attacker markup cannot carry a nonce the page's own policy lists. */
        {
            const char *poc = g_sinks[i].poc;
            int allowed = sc->policy < 0
                              ? policy_allows_string_compilation(pc)
                              : policy_allows_inline(pc, (CspInlineType)sc->policy, NULL, poc,
                                                     poc ? strlen(poc) : 0);

            if (!allowed) {
                json_buf_raw(&b, ","); json_buf_key(&b, "cspBlocks");
                json_buf_str(&b, policy_container_csp(pc));
            }
        }
        /* AND THE SAME RULE ONE ALGORITHM EARLIER. Under `require-trusted-types-for 'script'` an innerHTML
           assignment THROWS before the markup is ever parsed, so the model's breakout is real and the write
           that carries it never happens on the real page. trusted_types.c has answered this question all
           along and nothing asked it. The emitted value is the SINK GROUP the CSP names, because that is what
           the directive is written in terms of and what a reader has to add a policy for. */
        if (sc->tt >= 0 && trusted_types_required(ctx, (TrustedTypeKind)sc->tt)) {
            json_buf_raw(&b, ","); json_buf_key(&b, "trustedTypes");
            json_buf_str(&b, "script");
        }
        /* THE DELIVERY — including whether this is §S(b)'s TWO-STAGE plant-then-load PoC. There is deliberately
           no separate `stored` boolean: "is it stored" is not a second fact beside the mechanism, it IS the
           mechanism (a `plant` delivery is two-stage and every other one is a single load), and two fields for
           one fact is precisely the drift that made five names on this record mean nothing. */
        /* WHAT THE SOLVE COST, because success is the one event that destroys the record of it. A search that
           fires stops emitting the parked shape — `solved()` skips it below — so `tried`, `turns`, `survived`
           and `escaped` all vanish at the instant they become a fact about a REAL exploit rather than about a
           search in progress. A reader then cannot tell a PoC that fell out of the first written-down vector
           from one that took forty candidate runs and a derivation, which is the difference between a sink
           anyone would have found and a sink only this tool finds.
           IT IS THE COUNT AND NOT A FLAG, and one number rather than four: `tried` is the whole search's cost
           in document re-runs, which is what the other three are each a component of, and the parked shape
           already states all four for every search that has not solved. A fired entry needs the total, not the
           breakdown of a question that is now answered.
           ABSENT IS IMPOSSIBLE HERE, which is what makes it a positive statement: record_sink CHECKs that a
           finding's twin is on the pending list, so a fired entry always has a search behind it to ask. */
        {
            const Cand *tw = search_of(g_sinks[i].source, g_sinks[i].cls);
            char t[32], dv[64];
            CHECK(tw != NULL,
                  "solve: a fire-verified @S finding is being emitted with no search behind it — record_sink "
                  "asserts the twin at the moment the PoC is stored, so an absent one here means the pending "
                  "list was rewritten under a finding and the report is about to state a cost it cannot read");
            json_buf_raw(&b, ","); json_buf_key(&b, "searched");
            snprintf(t, sizeof t, "%d", tw->tried); json_buf_raw(&b, t);
            /* AND THE MEASURED CONSTRAINT THE PoC WAS BUILT UNDER, from the same search. On a FIRED entry it
               is what says which bytes the exploit is allowed to contain, so a reader reproducing it by hand
               knows which of them the browser would have eaten — a fact the payload alone does not carry. */
            emit_delivery(&b, g_sinks[i].root, cand_delivers(tw, dv, sizeof dv));
        }
        json_buf_raw(&b, "}");
    }
    for (int i = 0; i < g_pending_n; i++) {
        char t[32];
        if (solved(g_pending[i].sink, g_pending[i].src)) continue;
        if (n++) json_buf_raw(&b, ",");
        json_buf_raw(&b, "{"); json_buf_key(&b, "sink"); json_buf_str(&b, sink_name(g_pending[i].sink));
        json_buf_raw(&b, ","); json_buf_key(&b, "source"); json_buf_str(&b, g_pending[i].src);
        json_buf_raw(&b, ","); json_buf_key(&b, "search"); json_buf_str(&b, "parked");
        json_buf_raw(&b, ","); json_buf_key(&b, "tried");
        snprintf(t, sizeof t, "%d", g_pending[i].tried); json_buf_raw(&b, t);
        /* …AND HOW MANY OF THOSE RUNS CAME BACK FROM A PREVIOUS SESSION, which is the term solve.h's own
           arithmetic for `tried` names and which no field carried. `tried` is the entries not marked
           `withdrawn` PLUS the candidates resumed out of the cold tier, and a reader given only the first term
           cannot perform it: `tried:6` beside `payloads:[]` reads identically as a cross-session search whose
           every run was rebuilt from a park record and as a producer that dropped a field.
           IT IS ALSO THE ONE FACT THAT MAKES `reached` READABLE BESIDE A PROBES-ONLY LIST, which is why it is
           emitted here rather than left as prose. "A search holding nothing but probes cannot have had a
           breakout ARRIVE" is true of a search whose every run is a row in that list and false the moment one
           of them is a resumed candidate — its marker-carrying bytes ride the FLOW and are in no row. The
           consumer states that from this number instead of inferring it from `tried` against `payloads` and
           `withdrawn`, which would be a view re-deriving a producer fact it cannot check.
           UNCONDITIONAL, AND 0 IS THE LOAD-BEARING VALUE: it is the positive statement that every run this
           search has had is one of the rows below, which is what licenses every implication read off them. */
        json_buf_raw(&b, ","); json_buf_key(&b, "resumed");
        snprintf(t, sizeof t, "%d", g_pending[i].resumed); json_buf_raw(&b, t);
        /* …AND HOW MANY CAME BACK AND WERE REFUSED BEFORE THEY COULD RUN, which is the state the pair above
           cannot express. A parked record carries bytes an EARLIER session derived, and this build narrows the
           delivery table from the root's carrier declaration the moment the root arrives, so a record written
           before that narrowing names a payload this build positively contradicts and never runs
           (solve_resume_candidate). Neither `tried` nor `resumed` moves for it — both count RUNS — so without
           this number a search whose every parked candidate was refused reports `tried:0,resumed:0` exactly as
           a search nothing was ever parked for, and that reading is the positive statement (solve.h) that
           every run this search has had is a row in `payloads`. The two take opposite actions: the first says
           this build's carrier rules have moved past a stored recipe, the second says the frontier never
           reached here. UNCONDITIONAL, and 0 is the load-bearing value: it is what says the residue and this
           build still agree about what the source can carry. */
        json_buf_raw(&b, ","); json_buf_key(&b, "resumedWithdrawn");
        snprintf(t, sizeof t, "%d", g_pending[i].resumed_withdrawn); json_buf_raw(&b, t);
        /* …AND HOW MANY OF THOSE RUNS GOT HERE, which is the half `tried` cannot state (see the field). The
           two together are the only thing that tells a document nobody has explored far enough apart from a
           breakout that arrived and did not work — `reached:0` is the first, anything else is the second. */
        json_buf_raw(&b, ","); json_buf_key(&b, "reached");
        snprintf(t, sizeof t, "%d", g_pending[i].reached); json_buf_raw(&b, t);
        /* …AND HOW MANY TURNS THE SCHEDULER HAS GIVEN IT, which is what makes `reached:0` readable: with
           `turns:0` this search's candidates have never once held the thread, and with `turns:N` they have run
           and have not got as far as the sink. One is a WFQ question and the other is a distance question. */
        json_buf_raw(&b, ","); json_buf_key(&b, "turns");
        snprintf(t, sizeof t, "%d", g_pending[i].turns); json_buf_raw(&b, t);
        /* …AND THE TWO OBSERVATION COUNTS, WHICH ARE WHAT SPLIT `turns:N,reached:0,survived:0` INTO THE THREE
           STATES IT HAS ALWAYS BEEN. `turns` made `reached:0` readable by separating a search the WFQ has
           never served from one whose flows have run; these separate the second of those into the three things
           it was still saying at once, each taking different work:
             `substituted:0`                      — the runs ended before their own SOURCE READ. A question
                                                    about the PATH: a gate in front of the source is turning
                                                    these flows away, and nothing here is about the payload.
             `substituted:D, sinkStrings:0`       — the bytes entered the program and no code-execution sink
                                                    ran at all while they were live. The distance question.
             `sinkStrings:S, survived:0`          — S sinks EXECUTED and not one byte of the candidate was in
                                                    any of the strings they were handed. A question about the
                                                    PAYLOAD's own transform or routing, which is the opposite
                                                    instruction to the line above it.
           BOTH ARE UNCONDITIONAL AND 0 IS A REAL VALUE EACH MUST BE ABLE TO SAY — the zero is the load-bearing
           reading in both cases, so there is no absence here to read positively and an omission would be the
           defect rather than a statement. Neither is a rung: nothing about the WFQ moves at either write, and
           §@S(ii)'s ledger and the flow's comparator are untouched by both.
           `sinkStrings` COUNTS STRINGS AND NOT ARRIVALS, which is the difference between it and `reached`:
           `reached` is a BREAKOUT of this search turning up at its OWN sink, and this counts every string any
           code-execution sink was handed while this search's substitution was live, whether or not a byte of
           the candidate was in it. A reader that took one for the other would read `sinkStrings:400` as four
           hundred arrivals. */
        json_buf_raw(&b, ","); json_buf_key(&b, "substituted");
        snprintf(t, sizeof t, "%d", g_pending[i].substituted); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "sinkStrings");
        snprintf(t, sizeof t, "%d", g_pending[i].sink_strings); json_buf_raw(&b, t);
        /* …AND THE RUNG BENEATH BOTH OF THEM, WHICH IS THE ONE `substituted:0` COULD NOT SAY ANYTHING ABOUT.
           The three states above all begin at or past the source read; this one is the approach to it, and
           it is what splits `substituted:0` — a positive statement that these runs ended before their own
           source read — into the two things it has been saying at once:
             `runwayPerMille:0`     — the candidates were given the thread and consumed NONE of their own
                                      recorded path. A question about what turns a replay back at its first
                                      arm, and nothing about the distance to the source.
             `runwayPerMille:~1000` — they consumed the whole of it and the source read is still ahead. The
                                      distance question, and a statement that the fitness rung below
                                      FLOW_RUNG_DELIVERED is SATURATED and is directing nothing further.
           THOUSANDTHS, WITH THE UNIT IN THE KEY, for the reason the field's own declaration gives: every
           other number on this entry is a count, and a bare `runway` would be read as one.
           UNCONDITIONAL AND 0 IS THE LOAD-BEARING READING, exactly as for the two above — a search whose
           candidates have never been switched in reports 0 here beside `turns:0`, and one whose candidates
           ran and replayed nothing reports 0 beside `turns:N`. Those are told apart by `turns`, which is why
           this field is emitted next to it and not instead of it.
           AND THE THIRD READING OF THE SAME 0 IS TOLD APART BY `runwayArms` BELOW, which is emitted with it
           and never without it: `turns` says whether the candidates RAN, and only the arm count says whether
           there was anything for them to replay when they did. A 0 here over `runwayArms:0` is a search whose
           detection decided no branch at all, and the number is then a tautology rather than a measurement —
           a reader who acts on it hunts a gate that refuses an arm for a search that was never offered one. */
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayPerMille");
        snprintf(t, sizeof t, "%d", g_pending[i].replay_pm); json_buf_raw(&b, t);
        /* THE ARM COUNT THE FRACTION ABOVE IS SILENT ABOUT — see the field for what each of its two readings
           means and why it is NOT that fraction's denominator. Emitted IMMEDIATELY beside it, because the
           whole of what it adds is a joint reading and a consumer that finds one without the other is back to
           the merged zero this pair exists to split. */
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayArms");
        snprintf(t, sizeof t, "%d", g_pending[i].reinject_len); json_buf_raw(&b, t);
        /* AND THE POSITION AS ITS TWO HALVES, WHICH IS WHAT THE THOUSANDTHS ABOVE ROUND AWAY. `runwayWalked:0`
           beside `runwayPerMille:0` is a replay that consumed no arm; `runwayWalked:3` beside the same 0 is
           three arms out of a path long enough that one part in a thousand does not resolve them. `runwayOf`
           is the denominator of THAT reading — `dec_total()` when it was taken — and is not `runwayArms`,
           which is the frozen path every candidate starts on; below the delivery
           `runwayPerMille == round(runwayWalked/runwayOf*1000)` and past it the two diverge because the
           thousandths carry flow_observe_rung's pin and the pair never does. */
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayWalked");
        snprintf(t, sizeof t, "%ld", g_pending[i].replay_arms); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayOf");
        snprintf(t, sizeof t, "%ld", g_pending[i].replay_of); json_buf_raw(&b, t);
        /* …AND THE TWO MIDDLE RUNGS, WHICH IS WHAT SPLITS `reached:0` AND `reached:N` INTO THE FOUR STATES THEY
           REALLY ARE. `survived`/`survivedOf` is the FURTHEST any candidate of this search has got its own
           bytes through the page's own transforms to ANY sink, so `turns:900,reached:0,survived:11,
           survivedOf:14` is a FILTER eating the candidate eleven-fourteenths of the way in — the same report
           before, opposite work.
           `survived:0` IS NOT "A DOCUMENT NOBODY HAS EXPLORED FAR ENOUGH", WHICH IS WHAT THIS PARAGRAPH USED
           TO SAY AND WHAT THE PAIR ABOVE NOW REFUTES. A best-so-far is silent about how many times it looked,
           so a zero here is read WITH `substituted` and `sinkStrings` or not at all: `substituted:0` is a path
           that never reached the source read, `sinkStrings:0` is the distance reading this sentence claimed
           for all three, and `sinkStrings:X,survived:0` is X sinks that executed carrying none of the payload
           — a question about the payload, and the opposite instruction.
           `escaped` is how many arrivals reached an EXECUTABLE position, which `reached` could not say and
           which `fires` only approximates: `fires` counts every auto-firing handler in the parse INCLUDING the
           page's own markup, so a card reading it alone stated "none reached an executable position" as a fact
           about the payload on evidence that was partly about the template around it.
           BOTH ARE UNCONDITIONAL, and 0 is a real value each of them must be able to say (nothing of any
           candidate has been seen at any sink; nothing has got out of its context). `survivedOf:0` beside
           `survived:0` is the same statement said once — no observation has been recorded — and not a length
           this file failed to write. */
        json_buf_raw(&b, ","); json_buf_key(&b, "survived");
        snprintf(t, sizeof t, "%d", g_pending[i].surv_run); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "survivedOf");
        snprintf(t, sizeof t, "%d", g_pending[i].surv_len); json_buf_raw(&b, t);
        /* WHICH SEGMENT LIVED AND WHERE IT LANDED — the two facts the pair above measures and cannot state,
           and the ones §@S(2) names as the input to the mutation that follows a near miss ("which bytes
           survive to which positions", "which segment died and where the rest landed"). `survivedAt` is the
           offset into the CANDIDATE where the recorded run begins; `survivedTo` is where that run was found in
           the string the sink was handed.
           IT SPLITS ONE READING INTO TWO OPPOSITE INSTRUCTIONS. `survived:11,survivedOf:14,survivedAt:0` is a
           payload whose TAIL the page cut — the escape opened and its terminator never arrived — and
           `survived:11,survivedOf:14,survivedAt:3` is one whose HEAD it ate, so the escape never opened at
           all. Same three-byte gap, opposite mutations of opposite segments, and until now one report.
           ABSENT WHEN NO RUN HAS BEEN RECORDED, decided on the RUN rather than on the offset, because 0 is a
           real and common offset: emitting it for a search that has observed nothing would state that a run
           nobody has seen begins at the candidate's first byte. Same shape and same reason as `fires` and
           `witnessed` — the absence is the positive statement, never a zero standing in for one. */
        if (g_pending[i].surv_run > 0) {
            json_buf_raw(&b, ","); json_buf_key(&b, "survivedAt");
            snprintf(t, sizeof t, "%d", g_pending[i].surv_at); json_buf_raw(&b, t);
            json_buf_raw(&b, ","); json_buf_key(&b, "survivedTo");
            snprintf(t, sizeof t, "%d", g_pending[i].surv_out); json_buf_raw(&b, t);
        }
        json_buf_raw(&b, ","); json_buf_key(&b, "escaped");
        snprintf(t, sizeof t, "%d", g_pending[i].escaped); json_buf_raw(&b, t);
        /* AND WHAT WAS ACTUALLY TRIED, which three counts cannot say. `tried` is how many runs, `reached` how
           many arrived and `turns` how many turns the scheduler gave them — all quantities, and the state this
           search is most often in wants a STRING: a breakout that ARRIVED and did not fire is a question about
           the bytes, and the reader has to see them to answer it. Without this the report says a search ran
           five candidates and never says what any of them was, which is the same silence `parked, tried 5`
           carried before the derivations replaced the fixed lists.
           ENTRY 0 OF A DERIVED CLASS IS THE INERT CONTEXT PROBE and is emitted like the rest, because it IS
           one of the runs `tried` counts and hiding it would make the list disagree with the count. What tells
           it apart is that it carries no marker: a probe cannot fire, by construction.
           These are the payloads as the SEARCH built them, never as the browser delivers them — the source's
           own transform is already stated once, beside this, as `sourceEncodes` and `deliveryPrefix`, and
           writing the delivered form here as well would be the same fact in two places, free to disagree. */
        /* AND WHETHER A FIRE WAS EVER QUEUED — for the classes that queue one, which is where the number can
           mean anything. ARRIVING at a sink is not reaching an EXECUTABLE position: html_fire parses the
           DELIVERED bytes and queues a program only if the parse put the marker in an auto-firing handler,
           and url_fire only if the delivered address survived as a `javascript:` URL. So `reached:1,fires:0`
           says the source's own transform defeated this breakout — the fragment set encodes `<`, the markup
           parses as text, and there is nothing executable to run — while `reached:1,fires:1` says the program
           EXISTS and has not been run yet, which belongs to the flow's sequence and not to this file. Those
           take opposite work and were one report.
           ABSENT FOR AN EVAL SINK, and the absence is the positive statement this record's other optional
           fields already use: that class's sink evaluates its own argument, so there is no queue to count and
           a `0` there would read as "nothing executable" when it means "nothing to queue". */
        if (sink_class(g_pending[i].sink)->queues_fire) {
            json_buf_raw(&b, ","); json_buf_key(&b, "fires");
            snprintf(t, sizeof t, "%d", g_pending[i].fires); json_buf_raw(&b, t);
        }
        /* WHETHER THE CONTEXT PROBE EVER GOT THERE — the producer fact that splits `probes == payloads` into
           the two opposite things it has been saying at once, and the last of `reached:0`'s readings with no
           number behind it.
           A DERIVED CLASS BUILDS NOTHING UNTIL ITS PROBE ARRIVES, because the state is read off the string a
           REAL run handed the sink and there is no other observation to read one off. So `probes == payloads`
           is TWO facts: the probe has not reached the sink yet (a distance-through-the-document question, the
           same one `turns` and `survived` are asked for), or it reached it and the derivation constructed no
           escape — which for a percent-encoded source is the CORRECT and final answer and is the whole point of
           solving the three observations jointly. Those take opposite work and read identically, which is
           precisely the state §@S forbids an instrument to leave a reader in — and the tell it names, a rung
           whose ABSENCE and whose ZERO read alike, was exact here: this quantity was computed on every search
           (`nwit`), read by the re-derivation, and emitted nowhere at all.
           IT COUNTS DISTINCT SINK WRITES AND NOT RUNS, because that is what the search holds: a page that
           writes the source into a sink from two templates is two contexts and two derivations, and a page that
           writes the same template twice is one (learn_witness dedups by text). So `witnessed:2` beside
           `payloads` of length 2 says two contexts were read and neither could be left.
           ABSENT FOR A SINGLE-CONTEXT CLASS, on the column that decides it rather than on a count: that class
           states its vectors at detection and runs no context probe at all, so a `0` there would read as "the
           probe never arrived" about a search that has none. Same shape and same reason as `fires`. */
        if (sink_class(g_pending[i].sink)->derive != SINK_DERIVE_NONE) {
            json_buf_raw(&b, ","); json_buf_key(&b, "witnessed");
            snprintf(t, sizeof t, "%d", g_pending[i].nwit); json_buf_raw(&b, t);
        }
        /* …AND THE OTHER PROBE'S ARRIVAL COUNT, BESIDE IT, BECAUSE THE PAIR IS THE SAME QUESTION ASKED OF THE
           SEARCH'S TWO INSTRUMENTS. `witnessed` says the CONTEXT probe reached the sink; this says the DELIVERY
           probe did. Without it `sourceDelivers` carried two opposite states under one absence: the probe has
           not run (wait for the scheduler), and the probe RAN, ARRIVED, and the page destroyed every token it
           was made of (stop deriving — the source's own transform is the whole answer, and it is the strongest
           thing a parked search can report). Those take opposite work, and the smoke's own `s-park-nodeliver`
           and `s-attr-nodeliver` rows read 0 for both.
           IT DOES NOT MOVE `sourceDelivers`' GATE, which stays on `deliv_seen`, because a table narrowed by
           nothing IS the permissive one: emitting it after a run that observed no byte would state that every
           declared byte arrives, which is the defaulted-field defect in the direction that fabricates. Two
           facts, two fields — the run happened, and something was learned from it.
           ASKED OF THE ENTRIES THE SEARCH HOLDS (cand_has_delivery_probe), so a search with no delivery probe
           is ABSENT here rather than zero, for the reason `witnessed` is absent for a single-context class. */
        if (cand_has_delivery_probe(&g_pending[i])) {
            DCHECK(!g_pending[i].deliv_seen || g_pending[i].deliv_runs > 0,
                   "an @S search reports a delivered byte observed while its delivery probe has never reached "
                   "a sink — observe_delivery counts the arrival before it looks for a single token, so a byte "
                   "learned without one is a second door into the delivery table, and `sourceDelivers` would "
                   "be a constraint no run of this search measured");
            json_buf_raw(&b, ","); json_buf_key(&b, "deliveryProbed");
            snprintf(t, sizeof t, "%d", g_pending[i].deliv_runs); json_buf_raw(&b, t);
        }
        /* HOW MANY OF `payloads`' ENTRIES ARE PROBES — counted off the entries' own labels, which is the
           producer fact that splits `reached:0` one more time and the one state of this search the report
           could not say at all. `payloads.length > probes` is exactly "this search has constructed an escape",
           the question `cand_has_escape` answers and queue_derived's hand-off asserts; it was never emitted,
           so a reader had the two halves of the question and not the question.
           IT IS NO LONGER A LEADING COUNT, and the emitted number is unchanged by that: probes are pushed when
           detection opens the search and nothing else pushes one, so they still occupy the leading positions —
           what changed is that the report now states what the entries ARE rather than where they sit, and a
           producer that ever pushed out of that order would be reported correctly instead of silently
           relabelling everything after it.
           The consumer must not re-derive it: the probe is told apart by carrying no marker,
           the marker vocabulary is this engine's, and deciding it from POSITION would be a view restating a
           producer fact it cannot check — which is why popup-security.js declines to, twice, in its own words.
           WITHOUT IT A MEASURED PAGE IS DESCRIBED WRONG, not merely described thinly. An `innerHTML` sink fed
           the RAW fragment seeds two probes and NO escape, because the fragment percent-encode set holds the
           bytes every escape needs and the delivery probe measures that none arrives — the correct answer, and
           the derivation's whole point. The card computed from `turns>0, reached:0, survived:14/14` then told
           the reader the breakout had "not re-traversed the document yet" or "was cut down by the page's own
           FILTER": two questions, both false, and the true one — nothing was ever built to arrive — absent.
           EMITTED UNCONDITIONALLY, because 0 is a real value it must be able to say: a single-context class
           states its written-down vectors at detection and has no probe at all, so `probes:0` beside a
           non-empty list is the positive statement that every entry is an attack. */
        json_buf_raw(&b, ","); json_buf_key(&b, "probes");
        snprintf(t, sizeof t, "%d", cand_probes(&g_pending[i])); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "payloads"); json_buf_raw(&b, "[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_raw(&b, ",");
            json_buf_str(&b, g_pending[i].pl[c].bytes);
        }
        json_buf_raw(&b, "]");
        /* …AND HOW FAR EACH OF THOSE PAYLOADS GOT, one entry per `payloads` entry and in the same order, so a
           reader lines the two up by index rather than by guessing. It is the column `survived` cannot have:
           the ratchet saturates the moment ANY candidate lands intact, so `survived:16 survivedOf:16` beside
           `reached:0` says a full-length run of SOMETHING arrived and nothing about WHICH — and for a derived
           class the two candidates differ in nothing except these bytes, so that is the entire question.
           `[14,0]` says the inert probe's bytes reached a sink and the breakout built from them never did;
           `[14,9]` says the breakout travelled too and something after arrival is the problem. Those take
           opposite work and `survived` alone reports them identically. */
        json_buf_raw(&b, ","); json_buf_key(&b, "survivedBy"); json_buf_raw(&b, "[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_raw(&b, ",");
            snprintf(t, sizeof t, "%d", g_pending[i].pl[c].surv); json_buf_raw(&b, t);
        }
        json_buf_raw(&b, "]");
        /* …AND WHICH OF THEM THE SEARCH'S OWN MEASUREMENT HAS SINCE WITHDRAWN, one entry per `payloads` entry
           and in the same order as the two lists above it.
           IT EXISTS BECAUSE THE WITHDRAWAL WOULD OTHERWISE BE A SILENT DROP, and a silent drop on this record
           reads as the opposite of what happened. A withdrawn entry is never seeded, so it never raises
           `tried` and its `survivedBy` column stays 0 — which is byte-for-byte the report of a breakout that
           WAS run and got nowhere. Those are opposite verdicts: one says the page's code ate the candidate and
           the search should keep looking, the other says the SOURCE cannot carry these bytes at all and the
           search correctly declined to spend a document re-run on them. §@S names that tell exactly — a rung
           whose absence and whose zero read alike — and it applies to a payload's row as much as to a count.
           IT IS ALSO WHAT KEEPS `tried` AND `payloads` READABLE TOGETHER. solve.h says the probes are listed
           because omitting them "would make the list disagree with the count"; a withdrawal makes them
           disagree in the other direction, and this column is the arithmetic that reconciles it — `tried` is
           the entries not marked here (plus any candidate this session resumed out of the cold tier, whose
           bytes ride the flow and have no row).
           THE PRODUCER STATES IT AND THE CONSUMER MAY NOT RE-DERIVE IT, for the reason `probes` gives one
           field up: the constraint is a MEASURED table held on this search, a reader has no access to it, and
           reading a withdrawal off the payload's SHAPE would be a view restating a producer fact it cannot
           check. A `1` is a positive statement about the SOURCE's transform; `0` is a positive statement that
           this spelling is still on the table. */
        json_buf_raw(&b, ","); json_buf_key(&b, "withdrawn"); json_buf_raw(&b, "[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_raw(&b, ",");
            json_buf_raw(&b, (g_pending[i].pl[c].kind == CAND_ESCAPE &&
                               !solve_delivered_ok(&g_pending[i].deliv, g_pending[i].pl[c].bytes)) ? "1" : "0");
        }
        json_buf_raw(&b, "]");
        /* The parked entry carries the DECLARATION, not the envelope: a search that has not solved has no
           vector to state and no PoC to reproduce, so `firesOn`/`cspBlocks`/`trustedTypes` would be claims
           about a PoC that does not exist. What it does carry is the whole source declaration — the bytes a
           candidate must survive AND how the attacker would have to reach the victim if one ever fires. */
        {
            char dv[64];
            emit_delivery(&b, g_pending[i].root, cand_delivers(&g_pending[i], dv, sizeof dv));
        }
        json_buf_raw(&b, "}");
    }
    json_buf_raw(&b, "]");
    return json_buf_take(&b);
}

int solve_count(void) { return g_sinks_n; }

void solve_free(void) {
    /* THE SEAM IS GIVEN BACK FIRST, and it is an ownership fix rather than tidiness: the engine announces every
       program evaluation for as long as a hook is installed, and everything below this line frees the store
       add_pending writes into. A page that evals after the agent's release would be detected into freed
       memory. NULL is the registration's own word for "no host is listening", which is exactly what this host
       becomes here. */
    JS_SetEvalSinkHook(NULL);
    for (int i = 0; i < g_pending_n; i++) {
        for (int c = 0; c < g_pending[i].npl; c++) free(g_pending[i].pl[c].bytes);
        free(g_pending[i].pl);
        /* THE CONTEXT WITNESSES — one owned copy per distinct sink write, kept for the re-derivation a changed
           delivery observation performs, so they live exactly as long as the search does. */
        for (int c = 0; c < g_pending[i].nwit; c++) free(g_pending[i].wit[c]);
        free(g_pending[i].wit);
        /* THE SEGMENT REFERENCE THE SEARCH STILL HOLDS — a search that never solved still has its probe's
           re-injection blob, and the frozen chain under it is freed only when the last reference goes. */
        if (g_pending[i].reinject) decide_blob_free(g_pending[i].reinject);
        free(g_pending[i].src);
        free(g_pending[i].root);
    }
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
    for (int i = 0; i < g_sinks_n; i++) { free(g_sinks[i].source); free(g_sinks[i].root); free(g_sinks[i].poc); }
    free(g_sinks); g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
}
