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
   spellings, indirect eval, `new Function`, ShadowRealm.prototype.evaluate — beside HTML 8.6's string handler,
   which timer.c announces because that arm is the host's. Before that registration existed this function had
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
     parked search  `{"sink":..,"source":..,"search":"parked","tried":N,"reached":M,"turns":T,"survived":S,
                      "survivedOf":L,"escaped":E[,"fires":F],"payloads":[..],"survivedBy":[..]
                      [,"sourceEncodes":".."][,"sourceDelivers":".."][,"delivery":".."][,"deliveryPrefix":"#"]}`
   The parked shape exists because absence is never a "safe" verdict: a sink an attacker source REACHES is
   reported whether or not its breakout has been solved, and it carries how far the search got plus the source's
   own declaration. There is deliberately no "verified":false — the entry states what was searched, never that
   the sink is safe. An array for the same reason the @H surface is one — result.h owns the document.

   "HOW FAR THE SEARCH GOT" IS TWO NUMBERS AND USED TO BE ONE THAT COULD NOT SAY IT. `tried` is raised where a
   candidate is SEEDED, so it is fixed the instant the flow is created; `reached` is raised where a BREAKOUT's
   own bytes ARRIVE at this sink. A search reporting `tried:5,reached:0` has five flows queued somewhere in the
   middle of a document nobody has explored far enough, and one reporting `tried:5,reached:5` had every
   breakout delivered and broke out of nothing — opposite verdicts, needing opposite work, which the single
   number reported identically.
   `reached` COUNTS BREAKOUTS AND NOT CONTEXT PROBES, and the distinction is the whole value of the field: a
   probe carries an inert locator and cannot fire by construction, so counting its arrival makes `reached:1`
   mean either "the probe got here and the breakout has not run" or "the breakout got here and failed" — the
   two readings the field exists to separate. A derived class's probe arrival is already stated by `payloads`
   holding more entries than the probes it opened with (its breakouts exist only because a probe returned
   them), so nothing is lost and nothing is stated twice.
   `turns` IS THE THIRD, AND IT IS WHAT MAKES `reached:0` READABLE AT ALL. Seeded, arrived and SCHEDULED are
   three different facts: `tried:2,reached:0,turns:0` is a search the WFQ has never once given the thread to,
   and `tried:2,reached:0,turns:900` is one whose flows have run and have not got as far as the sink. The first
   is a scheduling question and the second a distance-through-the-document one, they take opposite actions, and
   with two numbers they were the same report.
   `survived`/`survivedOf` AND `escaped` ARE THE TWO MIDDLE RUNGS, and they are the fields that make the other
   three actionable rather than merely different. §@S says the search is "DISTANCE-DIRECTED (a fitness of
   {filter-survived, sink-reached, context-escaped, handler-fires} the WFQ reads)"; only the second and fourth
   of those had an observation site, and BOTH of them are at or past the sink, so a candidate's reward was 0
   for its entire runway and nothing could prefer a near-miss to an unstarted flow.
   `survived` is the LONGEST CONTIGUOUS RUN of a candidate's own bytes that has ever been seen in a string a
   re-execution handed ANY code-execution sink, and `survivedOf` is the length of the candidate that achieved
   it — held as a pair rather than a ratio because the card has to be able to say WHICH numbers. A run and not
   a tally: an escape is a SEQUENCE, so twelve of its bytes scattered are worth nothing and four adjacent are
   worth something. It answers the question `reached:0` could not: `turns:900,reached:0,survived:0` is a
   document nobody has explored far enough, and `turns:900,reached:0,survived:11,survivedOf:14` is the page's
   own FILTER eating the candidate eleven-fourteenths of the way in. They took opposite work and were one
   report. §@S(2)'s other forms — dropped, escaped, re-encoded — are not separate numbers here on purpose: a
   byte the page re-encoded cannot break a sink out of its context, so for a FITNESS it is exactly "did not
   survive", and reporting `&lt;` as a surviving `<` is the false-PoC direction.
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
   this session's record does not hold — a cold-resumed one, whose bytes ride the resumed FLOW rather than the
   record, which is the same reason `payloads` can be empty beside a non-zero `tried` — contributes to
   `survived` and to no column here, because there is no row of this session's for it.
   `payloads` IS THE ONE FIELD THAT IS NOT A COUNT, and the search's most common state is the one
   that needs it: a breakout that ARRIVED and did not fire is a question about the BYTES, and no quantity
   answers it. The LEADING entries of a derived class are its probes — the inert context probe, and beside it
   the delivery probe where the source declares a percent-encode set for that probe to measure — and each is
   one of the runs `tried` counts, so omitting them would make the list disagree with the count; they are told
   apart by carrying no marker.
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
     `cspBlocks`    the page's serialized CSP, present only when it kills THIS vector. Absent = CSP §4.2.3's
                    inline check (or §4.4.1's, for `eval`) said Allowed.
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
                    payload rides the address the victim arrives FROM), "user-file". ABSENT = the source declared
                    none, which is what server-injected page state is: the attacker writes it directly and no
                    component carries or transforms it. A consumer states that; it never guesses a vector.
   The engine owns this whole vocabulary. A delivery layer switches on these tokens and may say it cannot
   PERFORM one, but it never decides which source uses which — that was a `{hash}|{search}|{pm}|{reply}` table
   in the offscreen matching a display shape this engine has never emitted, which is why live verify could not
   build a PoC for any finding it produces. */
char *solve_json_array(JSContext *ctx);
int   solve_count(void);

#endif
