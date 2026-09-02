/* @S SOLVER — the NOVEL contribution. When a concolic ATTACKER source reaches a code-execution sink, construct
 * a breakout DERIVED from the sink context and VERIFY it by FIRING (an X9 marker must actually call), not a
 * fixed-payload guess. The emitted finding IS a replay-verified PoC; absence is never a "safe" verdict, only
 * search-not-yet-solved.
 * WHERE EACH CLASS'S BREAKOUT COMES FROM IS A COLUMN OF THE SINK TABLE, and there are exactly two answers.
 * TWO of the three classes DERIVE it, each from the parser that owns the sink's own language and each from a
 * string a REAL run of the page produced: solve_html.c reads the §13.2.5 tokenizer state off the Lexbor parse
 * of the markup sink's output, and solve_js.c scans the eval sink's argument per ECMAScript §12 for the
 * lexical state the attacker's bytes are in. Both are fed by the same mechanism — a probe candidate run that
 * injects an inert locator at the source instead of a breakout.
 * The URL sink STATES it, and it is the only class left that may: the sink IS one context, so there is nothing
 * to derive — navigating executes the `javascript:` scheme and nothing else does. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_H
#define ENGINE_HOST_SOLVER_SOLVE_H

#include "quickjs.h"
struct Flow;

/* Install the X9 fire-marker + init the @S store. Call once at engine init (after the global exists). */
void solve_init(JSContext *ctx);
void solve_free(void);

/* eval(arg): a JS-context code-execution sink. innerHTML=arg: an HTML-context sink. Detection records the
   source; a candidate re-run executes/re-parses the injected value so firing can be observed. */
/* THE JS ONE IS REACHED DIFFERENTLY FROM THE OTHER TWO, and the difference is OWNERSHIP rather than taste. A
   markup sink and a URL sink are HOST operations, so the browser component that performs one calls its
   detector directly. `eval` is not: 19.2.1 "eval ( source )" and 20.2.1.1.1 CreateDynamicFunction are
   ECMAScript intrinsics inside the engine, so this function is REGISTERED with it (JS_SetEvalSinkHook, from
   solve_init) and the engine announces every value offered to a program evaluation — direct eval in both its
   spellings, indirect eval, `new Function`, ShadowRealm.prototype.evaluate — beside HTML §8.7 Timers's string
   handler, which timer.c announces because that arm is the host's. Before that registration existed this function had
   ONE caller in the whole tree and it was the fixture, so solve_js.c's entire ECMAScript 12 derivation ran
   only when a test asked for it and a real page's `eval(prefix + attackerInput)` produced no finding.
   IT IS HANDED BOTH ARMS OF 19.2.1.1 STEP 2 and it needs both: unknown external input is not a String, so
   `eval(location.hash)` takes step 2 and is DETECTED here, while a candidate run's substitution makes the very
   same expression a real String whose bytes this function scans for its own locator and its own breakout.
   AND IT FIRES NOTHING — the engine compiles and runs those bytes the moment this call returns, which is the
   fire oracle §@S asks for. A modelled evaluation beside the real one runs every PoC twice. */
void solve_eval_sink(JSContext *ctx, JSValueConst arg);
void solve_html_sink(JSContext *ctx, JSValueConst arg);
void solve_url_sink(JSContext *ctx, JSValueConst arg);

/* …AND THE SAME PAIR OF DECISIONS FOR A HOST ALGORITHM THAT TURNS A STRING INTO CODE ITSELF. Announces `handler`
   at the JS-context sink UNCONDITIONALLY and returns the program TEXT (owned) that algorithm should compile, or
   JS_UNINITIALIZED when there is no program in it.
   THEY ARE ONE OPERATION BECAUSE SPELLING THEM APART IS THE DEFECT. Announcing and "is there a program here"
   are the same question asked twice, and a caller that asks them separately answers them differently: the arm
   that had no program announced and queued nothing, the arm that HAD one queued it and announced nothing, and
   the result was exactly half a search — detection worked and every candidate run after it was invisible, so
   the ladder's own honest record read `witnessed:0` for ever and the search parked at probes == payloads with
   nothing left to try. That is the same shape the engine's own seam states in reverse ("the announcement is
   unconditional and is not the alternative to running the program"), and the only way a caller cannot get it
   half right is for the program text to come OUT of the announcing operation.
   WHY A HOST ALGORITHM IS A STRING-TO-CODE SINK WITHOUT BEING AN ECMAScript EVAL. HTML §8.7 Timers's timer
   initialization steps put the whole string arm in step 9's task: substep 9.8.2 asserts the handler is a
   string, substep 9.8.3 performs "EnsureCSPDoesNotBlockStringCompilation(realm, « », handler, handler, timer,
   « », handler)" — the CSP gate a page opts into with `unsafe-eval` and the same one `eval` passes — and
   substeps 9.8.7-9.8.8 create a classic script from it and run it. The spec says the equivalence itself, in the
   note under substep 9.8.6.2: "The effect of these steps ensures that the string compilation done by
   setTimeout() and setInterval() behaves equivalently to that done by eval()." So the value is a JS-context
   sink for the same reason `eval`'s argument is, and it is the SINK_EVAL class: same CSP question, and
   substep 9.8.1.4's get-trusted-type-compliant-string over TrustedScript is the same Trusted Types question.
   AND IT IS NOT THE ECMAScript SEAM, WHICH IS WHY IT IS A SEPARATE ENTRY RATHER THAN A CALL INTO ONE. What
   §19.2.1.2 HostEnsureCanCompileStrings covers is §19.2.1.1 PerformEval step 5 and §20.2.1.1.1
   CreateDynamicFunction step 11, and §8.7 performs neither — it reaches CSP directly and then runs a plain
   classic script, whose compile is JS_EVAL_TYPE_GLOBAL — so the engine's own two-sided `is_sink`/`announced`
   assert, which fires when a compile that IS an eval arrives unannounced, would be answered YES by a program
   that is not one. This entry therefore announces and returns text and touches THAT latch not at all; it
   raises its own, below, whose consumer is the host site that compiles. (The reason written here used to be
   that "a host algorithm queues a task between the two", and that stopped being true when §8.7's create-and-run
   moved into step 9's task: the announcement and the compile are now adjacent statements of one stage. The
   reason that survives is the one about the eval TYPE, which is a fact about what the two latches ask.)
   STEP 2's DECISION IS THE SOLVER'S TOO. A concolic carrying a STRING example is a source (§solver: the triple
   rides the value and every operator runs the real operation on the concrete), so a handler this engine has an
   example for names a real program and is returned as one — the same answer the engine gives `eval`, which is
   what the note above requires. A concolic with no example, or one whose example is not a string, names no
   program: nothing is compiled and there is nothing to drop. */
JSValue solve_eval_sink_source(JSContext *ctx, JSValueConst handler);

/* …AND THAT SEAM'S COVERAGE, TAKEN ONCE PER PROGRAM THE HOST COMPILES OUT OF A STRING. Answers 1 iff the
   bytes about to be compiled are the ones the call above just returned, and CLEARS itself in the answering — so it is
   read into a local and asserted, never spelled inside a DCHECK, whose condition a release build does not
   evaluate at all and would leave raised for the next program.
   IT EXISTED BECAUSE THE ARM WAS ABOUT TO MOVE, AND THE ARM HAS MOVED — which is the case it was built for
   and not a reason to retire it. §8.7 Timers puts the whole string arm inside step 9's task (substep 9.8.3's
   CSP gate and substeps 9.8.7-9.8.8's create-and-run are the fire's steps, not the set's), and
   core/timing/timer.c performed them at the SET, queueing the program through a host edge whose own body took
   this latch. Relocation is exactly the shape in which an announcement gets left behind — the queue call
   travels, the detector call does not, and nothing breaks: the timer still fires, the page still runs, and
   every candidate run after it is silently invisible again. It did not happen, because the assert is at the
   CONSUMER: the consumer is whoever turns those bytes into a program, that is now the task machine's own
   JS_Eval, and the DCHECK moved to stand over it. A route added later FIRES here instead of quietly widening
   the old wrong answer. */
int solve_eval_sink_announced(void);

/* After detection, SEARCH breakout candidates for every recorded source: inject each at the source, re-run the
   REAL program (`rerun`), and record the first that FIRES as the replay-verified PoC. */
/* SEED one candidate flow per (detected sink, breakout) onto the ONE frontier — the re-fire is a FLOW, never a
   driver that runs the program to completion beside the BFS.
   IT IS A CURSOR, NOT A LATCH, and that is what a DERIVED breakout needs: a markup sink's breakouts do not
   exist when it is detected, they are constructed from a probe run that has to HAPPEN first, so the search
   grows after it has already been seeded once. Everything past the cursor is taken, so nothing is seeded twice
   and nothing appended later is missed — the two halves the old `if (tried) continue` could not both say. */
int  solve_seed_candidates(JSContext *ctx);   /* seeds whatever each search has added since last time */
/* How many candidate flows this document has seeded in total. Each one RE-RUNS the page, so this number times
   the page's cost is most of what an @S search spends — and it is what says whether a run got slower because
   there were more searches or because each search grew. */
int  solve_candidate_count(void);
/* WHAT HAPPENED UPSTREAM OF EVERY ENTRY BELOW — the three numbers that make an EMPTY @S surface a measurement
   instead of a silence. `reached` is how many times a code-execution sink was executed by an EXPLORATION flow
   (a candidate re-run's arrivals are this engine's own injected bytes and are excluded by construction, not by
   a test); `tainted` is how many of those arrivals carried an attacker-controlled value; `suppressed` is how
   many of THOSE the unforgeable-principal rule declined to open a search for.
   ALL THREE OR NONE, because each is uninterpretable alone. With the source-read count (solver/concolic.h) they
   separate the four states one empty array was the evidence for: no attacker source read at all; sources read
   but no sink ever executed; sinks executed and only the page's own strings arrived; and taint arriving at a
   sink whose check no cross-document attacker can satisfy — which is a POSITIVE result and was rendered as the
   same nothing. They are costs and not findings: the same document under a different schedule legitimately
   reaches a sink a different number of times, so the differential must not compare them. */
void solve_arrival_census(long *reached, long *tainted, long *suppressed);
/* THIS SEARCH'S BYTES JUST ENTERED THE PAGE'S OWN PROGRAM — reported by the component that performs the
   substitution (solver/concolic.c's concolic_deliver), because that is the only site on the runway that can
   state it: every other number a parked entry carries is observed AT a sink, and §@S(i) wants an observation
   strictly before the thing it is a distance to.
   IT IS THE REPORT'S COUNT AND NOT THE FLOW'S RUNG. flow_observe_rung records the same event as
   FLOW_RUNG_DELIVERED — the COMPARATOR, monotone per flow, re-earned across a park, read at the pick; this is
   a COUNT on the SEARCH, which orders nothing, outlives the flows that produced it, and is the only one of the
   two a reader ever sees. §@S(ii) is why they are two quantities and not one.
   CALLING IT IS NOT OPTIONAL FOR A SECOND SUBSTITUTION DOOR: solve.c's two sink entries assert that a search
   measuring bytes in a sink's string has recorded at least one substitution, so a door that does not report
   here fires there rather than making the report quietly say the runway was never left. */
void solve_observe_substitution(struct Flow *f);
void solve_flow_begin(struct Flow *f);
void solve_flow_end(struct Flow *f);

/* THE DELIVERY ROOT OF THE SEARCH A LIVE CANDIDATE BELONGS TO — the PARK's half of what the call below is the
   RESUME's half of, asked at the moment cold.c writes that candidate's recipe.
   THE ROOT IS A FACT ABOUT THE SEARCH AND NOT ABOUT THE FLOW, which is why a candidate does not carry one: it
   is inherited unchanged through every derivation, so one sink's N candidates have one root between them and a
   copy on each Flow would be N owned strings whose only content is that they are equal — plus a dup obligation
   at every clone, park and free site. The park DOCUMENT still writes one per record, and that is a different
   thing: a record is rebuilt alone, by a session that has nothing else, so it has to be whole.
   Aborts rather than answering for a candidate whose search this session does not hold, or for a search that
   never learned its root — this is the last moment either fact exists in this process. */
const char *solve_candidate_root(const char *src, const char *sink_name);

/* A PARKED @S CANDIDATE IS COMING BACK — the cold tier's one call, and it does the whole of what a resumed
   candidate needs this file to know. It answers the sink table's OWN pointer for `sink_name`, which is what
   re-binds a class that crossed the tier by NAME (a `cand_sink` is a pointer into static storage with no
   identity outside the session it was minted in), and it re-registers that sink as PENDING and already-TRIED
   once.
   ALL THREE OR NONE, WHICH IS WHY IT IS ONE CALL AND NOT THREE.
     - The pending list is rebuilt by DETECTION, and a VERIFYING flow does not detect (solve_eval_sink and its
       siblings take the candidate branch and never call add_pending). So a resumed candidate that finished
       before any exploration flow had re-reached its sink would hand record_sink a finding for a sink it has
       never heard of, and that assert would be right to fire.
     - And the COUNT has to move with it: `tried` is the whole of what makes solve_seed_candidates idempotent,
       so a resumed candidate that did not raise it would be seeded a SECOND time out of the table, and the
       frontier would grow by one duplicate per visit — precisely what the park's write-once assert exists to
       prevent, arriving through the other door.
     - And the ROOT arrives with them, because a resumed candidate opens its search HERE and a verifying flow
       never detects, so nothing else in the resuming session has a value to read one off. Without it the entry
       stood at NULL and every emit of that search — the parked entry AND a fire-verified PoC taken off it —
       hit emit_delivery's assert: the whole @RESULT aborted in dev, and in release the envelope rendered the
       silence that MEANS "no component carries these bytes to the victim" over a payload whose delivery the
       ended session knew exactly. It crosses as the 'c' record's own field; see cold.h's grammar.
   Returns the table's pointer; a name this build's table does not have is a residue written by a build whose
   sink classes this one no longer has, and it says so rather than resuming a search that cannot report. */
const char *solve_resume_candidate(const char *src, const char *root, const char *sink_name);

/* EVERY DETECTED SINK as a JSON ARRAY (caller frees). Two entry shapes, because a sink is in one of two states
   and they must never be confused:
     fire-verified  `{"sink":..,"source":..,"poc":..,"firesOn":..[,"cspBlocks":".."][,"trustedTypes":"script"]
                      ,"searched":N[,"sourceEncodes":".."][,"sourceDelivers":".."][,"delivery":".."]
                      [,"deliveryPrefix":"#"]}`
     parked search  `{"sink":..,"source":..,"search":"parked","tried":N,"resumed":R,"reached":M,"turns":T,
                      "substituted":D,"sinkStrings":X,"survived":S,
                      "survivedOf":L[,"survivedAt":A,"survivedTo":O],"escaped":E[,"fires":F][,"witnessed":W]
                      ,"probes":P,"payloads":[..],
                      "survivedBy":[..],"withdrawn":[..]
                      [,"sourceEncodes":".."][,"sourceDelivers":".."][,"delivery":".."][,"deliveryPrefix":"#"]}`
   The parked shape exists because absence is never a "safe" verdict: a sink an attacker source REACHES is
   reported whether or not its breakout has been solved, and it carries how far the search got plus the source's
   own declaration. There is deliberately no "verified":false — the entry states what was searched, never that
   the sink is safe.

   AND THERE IS ONE ARRIVAL THAT PRODUCES NEITHER SHAPE, which is the only kind of silence this surface has.
   §Attacker-sources: a check on an attacker's own PRINCIPAL that cannot be forged — an exact equality against
   `event.origin` — is unsatisfiable cross-origin and SUPPRESSES the finding. A flow that pinned one really did
   reach the sink, and its arm keeps running and keeps emitting endpoints; what it may not do is open a search,
   because the PoC that search would fire-verify is one no cross-document attacker can deliver, and §S(d) makes
   a PoC's envelope part of the PoC. It is not a "safe" verdict and is deliberately not reported as one: the
   parked shape means "searched this far and not solved", which is a different fact, and stating it here would
   describe a sink nobody can reach as one whose search is merely unfinished. The same sink stays reportable
   through any other flow that arrives without the demand — the equality's false arm, or a sibling gated by a
   prefix/substring check, which pins nothing and is exactly the forgeable case the rule SOLVES. An array for the same reason the @H surface is one — result.h owns the document.

   "HOW FAR THE SEARCH GOT" IS TWO NUMBERS AND USED TO BE ONE THAT COULD NOT SAY IT. `tried` is raised where a
   candidate is SEEDED, so it is fixed the instant the flow is created; `reached` is raised where a BREAKOUT's
   own bytes ARRIVE at this sink. A search reporting `tried:5,reached:0` has five flows queued somewhere in the
   middle of a document nobody has explored far enough, and one reporting `tried:5,reached:5` had every
   breakout delivered and broke out of nothing — opposite verdicts, needing opposite work, which the single
   number reported identically.
   `reached` IS A REPORT COUNTER AND NOT THE WFQ's CROSSING, and it used to be both. Every arrival raises it,
   including one by a spelling `withdrawn` marks — those bytes really did turn up at the sink, the marker
   survived and the escape did not, and hiding that would make the number a claim about viability rather than
   a count of arrivals. What is NOT paid for such an arrival is the LEDGER rung: it is a distance to FIRING,
   and a candidate the search has measured cannot arrive intact has travelled none of it, so paying it there
   spent the one-time crossing on an arm and left nothing for the spelling that can fire. The two questions are
   separate state inside solve.c for that reason.
   `reached` COUNTS BREAKOUTS AND NOT CONTEXT PROBES, and the distinction is the whole value of the field: a
   probe carries an inert locator and cannot fire by construction, so counting its arrival makes `reached:1`
   mean either "the probe got here and the breakout has not run" or "the breakout got here and failed" — the
   two readings the field exists to separate.
   `witnessed` IS WHERE A DERIVED CLASS'S PROBE ARRIVAL IS STATED, and this paragraph used to say the arrival
   "is already stated by `payloads` holding more entries than the probes it opened with (its breakouts exist
   only because a probe returned them)". That is an implication read BACKWARDS: breakouts do imply an arrival,
   and their ABSENCE implies nothing whatever, so `probes == payloads` was carrying two opposite facts under one
   spelling — the probe has not reached the sink yet, and it reached it and the state its bytes landed in has no
   exit this source can carry. The first is the same distance question `turns` and `survived` are asked for; the
   second is the joint solve's CORRECT and final answer for a percent-encoded source, and is what the markup
   sink fed a raw fragment reports. §@S forbids leaving those behind one number, and names the tell exactly:
   a rung whose absence and whose zero read alike. So the arrival is counted where it happens — one entry per
   DISTINCT sink write the probe was observed at, deduped by the string itself, because a page that renders one
   template twice read one context and a page with two templates read two.
   ABSENT FOR A SINGLE-CONTEXT CLASS, which runs no context probe at all: `0` there would state that a probe
   this class does not have failed to arrive. Same shape and same reason as `fires`.
   `turns` IS THE THIRD, AND IT IS WHAT MAKES `reached:0` READABLE AT ALL. Seeded, arrived and SCHEDULED are
   three different facts: `tried:2,reached:0,turns:0` is a search the WFQ has never once given the thread to,
   and `tried:2,reached:0,turns:900` is one whose flows have run and have not got as far as the sink. The first
   is a scheduling question and the second a distance-through-the-document one, they take opposite actions, and
   with two numbers they were the same report.
   `substituted` AND `sinkStrings` SPLIT THAT SECOND FACT INTO THE THREE IT WAS STILL SAYING AT ONCE, and they
   are the two OBSERVATION COUNTS beside the best-so-far quantities below. A ratchet reports the furthest
   anything got and is structurally silent about how many times it LOOKED, so `turns:900,reached:0,survived:0`
   was one number over three opposite instructions:
     `substituted:0`                — the runs ended before their own SOURCE READ, so the bytes never entered
                                      the page's program at all. A question about the PATH in front of the
                                      source: a gate is turning these flows away. Neither the payload, the
                                      filter nor the sink is implicated, and no field could say this before.
     `substituted:D,sinkStrings:0`  — the bytes DID enter the program and no code-execution sink has run while
                                      they were live. The distance-through-the-document question.
     `sinkStrings:X,survived:0`     — X sinks EXECUTED and not one byte of the candidate was in any string
                                      they were handed. A question about the PAYLOAD, and the opposite work
                                      from the line above it.
   §@S names that tell exactly — a rung whose ABSENCE and whose ZERO read alike — and `survived:0` was both.
   `substituted` IS OBSERVED AT THE SOURCE READ and is the only number on this entry whose site is not a sink,
   which is what §@S(i) asks for; `sinkStrings` is observed where the survival fraction is, class-independently,
   so it counts every code-execution sink write during this search's candidate runs and not only its own class's.
   BOTH ARE COUNTS AND NEITHER IS A RUNG: nothing about the WFQ moves at either write, so §@S(ii)'s ledger and
   the flow's comparator are untouched. `substituted` counts SUBSTITUTIONS rather than distinct candidates (a
   page that reads its source in a loop delivers many times in one run) and `sinkStrings` counts STRINGS rather
   than arrivals — `reached` is the one that counts a breakout arriving at its own sink. In both the ZERO is
   what is load-bearing, which is why both are emitted unconditionally and neither has an absent form.
   `survived`/`survivedOf` AND `escaped` ARE THE TWO MIDDLE RUNGS, and they are the fields that make the other
   three actionable rather than merely different. §@S says the search is "DISTANCE-DIRECTED (a fitness of
   {filter-survived, sink-reached, context-escaped, handler-fires} the WFQ reads)"; only the second and fourth
   of those had an observation site, and BOTH of them are at or past the sink, so a candidate's reward was 0
   for its entire runway and nothing could prefer a near-miss to an unstarted flow.
   `survived` is the LONGEST CONTIGUOUS RUN of a candidate's own bytes that has ever been seen in a string a
   re-execution handed ANY code-execution sink, and `survivedOf` is the length of the candidate that achieved
   it — held as a pair rather than a ratio because the card has to be able to say WHICH numbers. A run and not
   a tally: an escape is a SEQUENCE, so twelve of its bytes scattered are worth nothing and four adjacent are
   worth something. It answers the question `reached:0` could not: `turns:900,reached:0,survived:11,
   survivedOf:14` is the page's own FILTER eating the candidate eleven-fourteenths of the way in, where
   `reached:0` alone said only that no whole breakout arrived. They took opposite work and were one report.
   ITS OWN ZERO IS NOT A THIRD READING OF THAT KIND, and this paragraph used to give it one ("a document
   nobody has explored far enough"). A best-so-far cannot say how many times it looked, so `survived:0` is
   read with `substituted` and `sinkStrings` above or not at all — the sentence it used to carry alone is one
   of the three those two separate, and it was the wrong one for the other two. §@S(2)'s other forms — dropped, escaped, re-encoded — are not separate numbers here on purpose: a
   byte the page re-encoded cannot break a sink out of its context, so for a FITNESS it is exactly "did not
   survive", and reporting `&lt;` as a surviving `<` is the false-PoC direction.
   `survivedAt`/`survivedTo` ARE WHERE THAT RUN IS, AND THEY ARE THE HALF THE PAIR ABOVE STRUCTURALLY CANNOT
   SAY. `survived`/`survivedOf` is a SIZE, and every mutation of a near miss is a question about a POSITION:
   `survived:11,survivedOf:14,survivedAt:0` is a payload whose TAIL the page cut — the escape opened and its
   terminator never arrived — while `survived:11,survivedOf:14,survivedAt:3` is one whose HEAD it ate, so the
   escape never opened at all. Identical gap, opposite segments, opposite work, and one report until now.
   `survivedAt` is the offset into the CANDIDATE at which the recorded run begins (so what is outside it is
   what died) and `survivedTo` is where that run was found in the string the sink was handed — §@S(2)'s "which
   bytes survive to which positions" read literally, and §@S's named input to the step that follows a near miss
   ("which segment died and where the rest landed").
   THEY EXIST BECAUSE THE OBSERVATION ALREADY DID AND NOTHING READ IT. solve_filter_survival has always
   reported both, and its own two-sided assert RE-READS the bytes at them, so they were computed and verified
   on every observation and then discarded with the C local that held them — §@S: "an observation with a
   computed writer and no reader is not a mechanism". That is the mirror of the read-with-no-writer defect and
   is harder to see, because nothing was absent, nothing defaulted, and the numbers were real.
   ABSENT TOGETHER WHENEVER `survived` IS 0, decided on the RUN and never on the offset: 0 is a real and common
   offset — the commonest one — so a 0 emitted for a search that has observed nothing would state that a run
   nobody has seen begins at the candidate's first byte. Same shape and same reason as `fires` and `witnessed`.
   `escaped` is how many arrivals reached an EXECUTABLE position, which is the fact between ARRIVING and
   FIRING and which nothing measured. Each class answers it from its own language: the eval sink asks the same
   ECMAScript §12 "ECMAScript Language: Lexical Grammar" scan that built the escape whether the marker now
   BEGINS an input element; the markup sink reads the marker out of an auto-firing handler in the real parse it
   already runs (HTML §8.1.1 Introduction lists "event handler content attributes" among the mechanisms that
   cause author-provided executable code to run); the URL sink asks whether the delivered address survived as a
   `javascript:` one, which for a single-context sink IS the escape. It is NOT `fires`, which counts every
   auto-firing handler in the parse INCLUDING the page's own template markup — so `escaped:0` is a statement
   about the PAYLOAD and `fires:0` is only sometimes one. Every state the scan cannot decide answers 0: an
   escape is never claimed on a scan that could not be made, because a rung that over-claims promotes a
   candidate that cannot fire while one that under-claims merely leaves it where it was.
   `survivedBy` IS `survived` PER CANDIDATE, one entry per `payloads` entry and in the same order, and it
   exists because the ratchet that makes `survived` cheap is also what blinds it. `survived`/`survivedOf` is
   the search's BEST, so it saturates at a full-length run the instant ANY candidate lands intact and can no
   longer say WHICH one did — and for a derived class the candidates are an inert context PROBE and the
   breakouts built out of what that probe observed, two flows whose only difference is these bytes. So
   `survived:16,survivedOf:16` beside `reached:0` is exactly the reading the pair cannot resolve, and
   `survivedBy:[16,0]` resolves it: the probe's bytes reached a sink and the breakout's never did, which is a
   question about the SECOND traversal, while `[16,9]` would say the breakout travelled and something after
   arrival is the problem. A ZERO is a real value each entry must be able to say, and a candidate whose payload
   this session's record does not hold contributes to `survived` and to no column here, because there is no row
   of this session's for it. The COLD TIER is the one producer of such a candidate — a resumed one's bytes ride
   the resumed FLOW rather than the record, which is the same reason `payloads` can be empty beside a non-zero
   `tried` — and it is a fact about the PAYLOAD rather than about the flow: where this session's own derivation
   independently constructs the same string, that is one payload and therefore ONE row, which both flows write,
   exactly as two seeded flows carrying identical bytes would (push_breakout dedups by text for the same
   reason). What has no row is bytes this session never held, never the fact of having been resumed.
   `withdrawn` IS THE THIRD COLUMN OF THAT SAME TABLE, and it exists because the row it describes is otherwise
   the exact report of its own opposite. The deliverability table a breakout is constructed under is MEASURED,
   by a delivery probe that has to run, so it narrows AFTER escapes have already been queued: a spelling built
   while everything still delivered can be one this search's own observation later contradicts. Such a spelling
   is WITHDRAWN — never seeded, so it never raises `tried` and its `survivedBy` entry stays 0, which is
   byte-for-byte what a breakout that WAS run and travelled nowhere reports. Those are opposite verdicts taking
   opposite work: `survivedBy:0, withdrawn:0` is the page's own code eating a candidate the search should keep
   mutating, and `survivedBy:0, withdrawn:1` is the SOURCE's percent-encode set making this exit unsatisfiable
   by construction, which is the joint solve's correct and final answer for that spelling. It is also the
   arithmetic that keeps `tried` and `payloads` readable together — `tried` counts the entries NOT marked here,
   plus any candidate resumed out of the cold tier whose bytes this session's own search never constructed, and
   which therefore has no row at all. `resumed` IS THAT SECOND TERM, and until it was emitted this arithmetic
   was one a reader could not perform: the sentence named two quantities and the record carried one, so
   `tried:6` beside `payloads:[]` read identically as a cross-session search whose every run was rebuilt from a
   park document and as a producer that had dropped a field.
   `resumed` IS ALSO WHAT LICENSES THE IMPLICATIONS READ OFF THE THREE COLUMNS ABOVE. Each of them — a
   `survivedBy` zero, a `withdrawn` flag, and `probes == payloads` beside a non-zero `reached` — is a statement
   about the runs this search has had, and each is sound exactly while every one of those runs IS a row here. A
   resumed candidate is the one run that is not: its bytes ride the FLOW, so a search holding nothing but its
   own probes really can report a breakout ARRIVING, which is the opposite of what the probes-only reading
   says. `resumed:0` is therefore the positive statement that the columns are complete, and it is emitted
   unconditionally because that zero is the load-bearing value.
   WITHDRAWING IS PRUNING A CONTRADICTED ARM AND NOT A BOUND: no count, no age, no retry limit and no seen-set
   decides it — only whether a positive observation of this search's own says these bytes do not arrive, and
   the table narrows solely on evidence (an unobserved byte keeps its arm). A PROBE is never marked: the
   context probe is inert ASCII and the delivery probe is built OUT OF the bytes in question, so both are the
   instruments the measurement is made with rather than escapes made under it.
   `payloads` IS THE ONE FIELD THAT IS NOT A COUNT, and the search's most common state is the one
   that needs it: a breakout that ARRIVED and did not fire is a question about the BYTES, and no quantity
   answers it. The LEADING entries of a derived class are its probes — the inert context probe, and beside it
   the delivery probe where the source declares a percent-encode set for that probe to measure — and each is
   one of the runs `tried` counts, so omitting them would make the list disagree with the count; they are told
   apart by carrying no marker.
   `probes` IS HOW MANY OF THEM THOSE ARE, and it is what makes the sentence above READABLE rather than merely
   true. `payloads.length > probes` is exactly "this search has constructed an escape"; equal is the state
   nothing else in the record can express — the derivation RAN and built nothing, because the bytes an escape
   needs cannot arrive through this source at all. That is not a thin reading, it is the difference between
   two opposite instructions: an `innerHTML` sink fed the RAW fragment reports `turns:2, reached:0,
   survived:14, survivedOf:14`, from which a reader concludes the page's FILTER ate a breakout or the flows
   never re-traversed the document, and BOTH are false — the fragment percent-encode set holds every byte the
   escape needs, the delivery probe measured that none arrives, and the search correctly has nothing to try.
   The consumer may not re-derive it: the probe is told apart by carrying no marker, the marker vocabulary is
   this engine's, and reading it off the ORDER would be a view restating a producer fact it cannot check.
   UNCONDITIONAL, and `0` is a real value: a single-context class states its written-down vectors at detection
   and has no probe, so `probes:0` says every entry in the list is an attack.
   `fires` SEPARATES A SOLVER FAILURE FROM A SCHEDULING ONE, because ARRIVING at a sink is not reaching an
   EXECUTABLE position. A markup breakout becomes a program only if the real parse put its marker in an
   auto-firing handler and a URL one only if the delivered address survived as a `javascript:` URL, so
   `reached:1,fires:0` says the source's own transform defeated it (the fragment set encodes `<`, the markup
   parses as text) while `reached:1,fires:1` says the program exists and has not been run yet, which is the
   flow's sequence and not this file's. ABSENT for an eval sink: that class evaluates its own argument, so
   there is nothing to queue and a 0 would read as "nothing executable" when it means "nothing to count".

   THE FIRED ENTRY IS §S(d)'s REPRODUCTION ENVELOPE, and every field of it is a POSITIVE statement whose ABSENCE
   is equally positive — never a gap to be read as a default:
     `searched`     how many candidate runs the search took to fire — the one progress number that survives
                    success. A search that fires stops emitting the parked shape, so `tried`/`turns`/
                    `survived`/`escaped` all vanish at the moment they stop being about a search in progress
                    and start being the cost of a REAL exploit; without this a reader cannot tell a PoC that
                    fell out of the first written-down vector from one that took a probe, a derivation and
                    forty re-runs. ALWAYS present — every finding has a search behind it (record_sink asserts
                    the twin), so absence here would be a third state rather than a fact.
     `firesOn`      what makes the PoC RUN, off the sink's own fire oracle: "sink-evaluates" (the sink executes
                    the string where it stands), "parse-insert" (an auto-firing handler in markup, at insertion,
                    no interaction), "navigation" (a `javascript:` URL, when the navigation happens). ALWAYS
                    present — a PoC that does not say how it fires is not reproducible.
     `cspBlocks`    the page's serialized CSP, present only when it kills THIS vector. Absent = CSP §4.2.3
                    "Should element's inline type behavior be blocked by Content Security Policy?" (or, for
                    `eval`, §4.4.1 "EnsureCSPDoesNotBlockStringCompilation") returned "Allowed". The titles are
                    here because the numbers alone resolved to where the TERM "inline check" is DEFINED (§2.3
                    "Directives") rather than to the algorithm this sentence is about, which is exactly the
                    unfalsifiable state §Browser-half says a title exists to prevent.
     `trustedTypes` the CSP sink GROUP required at this sink, present only when the document requires one — the
                    assignment throws before the markup is parsed. Absent = no requirement applies, which
                    covers both "the document requires none" and "the standard makes this no TT sink".
     `sourceDelivers` THE MEASURED HALF OF `sourceEncodes`, and the two are different facts that a reader has
                    to be able to hold against each other. `sourceEncodes` is the DECLARATION — what the
                    component that owns the source states the browser percent-encodes on the way in — and it
                    is a PRIOR, not an outcome: a page that runs `decodeURIComponent` over its own fragment
                    receives the `<` the browser encoded, and this engine already fires a markup PoC through
                    exactly that round trip. This field is the subset of those bytes a RUN observed arriving at
                    a sink, taken off a delivery probe seeded beside the context probe (solve.c), and it is
                    what every derived escape is constructed under. An empty value beside a full declaration is
                    the strongest thing a parked markup search can say — the source's own transform is the
                    whole of the failure and no re-derivation reaches past it. ABSENT is TWO facts and both are
                    positive: the source declares no percent-encode set (nothing is in question), or no
                    delivery probe of this search has reached a sink yet (no measurement was taken). It is
                    never emitted from the permissive initial table, which would report "all of them arrive"
                    for a question nobody has asked.
     `delivery`     HOW an attacker puts bytes in this source, from the source's own declaration in the
                    component that owns it: "address" (the victim's own URL, at `deliveryPrefix`), "plant"
                    (§S(b)'s TWO-STAGE plant-then-load — there is no separate `stored` flag because being
                    stored is not a fact beside the mechanism, it IS the mechanism), "referring-address" (the
                    payload rides the address the victim arrives FROM), "user-file",
                    "cross-document-message" (HTML §9.3.3 "Posting messages": the attacker holds the victim
                    open in a document of their own and posts to it while it runs — no navigation and no
                    plant, and nothing transforms the bytes on the way in). ABSENT = the source declared
                    none, which is what server-injected page state is: the attacker writes it directly and no
                    component carries or transforms it. A consumer states that; it never guesses a vector.
   The engine owns this whole vocabulary. A delivery layer switches on these tokens and may say it cannot
   PERFORM one, but it never decides which source uses which — that was a `{hash}|{search}|{pm}|{reply}` table
   in the offscreen matching a display shape this engine has never emitted, which is why live verify could not
   build a PoC for any finding it produces. */
char *solve_json_array(JSContext *ctx);
int   solve_count(void);

#endif
