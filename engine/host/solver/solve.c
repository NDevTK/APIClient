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
   the sink CLASS, with nothing measuring it. */
typedef struct {
    char *src; char *root; int sink; int tried; int reached; int turns; int fires;
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
    int surv_run, surv_len; int escaped;
    /* THE BREAKOUTS THIS SINK'S SEARCH HAS, IN ORDER, AND HOW MANY OF THEM ARE ALREADY FLOWS. A derived-context
       sink does not KNOW its breakouts when it is detected — a probe run reads them off the sink's own parse —
       so the list GROWS after seeding has already happened, and a one-shot "this sink is seeded" latch cannot
       express that. A cursor can: seeding takes everything past it, so a breakout derived at any later moment
       is picked up by the next drain and none is ever seeded twice.
       TEXT, never an index into the class table: a candidate flow carries its payload to the cold tier and back
       (cold.c parks `cand_payload` as bytes for exactly this reason), so a recipe parked this session must
       still mean the same thing to a build whose tables have changed. */
    char **pl; int npl, plcap, seeded;
    /* HOW MANY OF `pl`'s LEADING ENTRIES ARE PROBES rather than escapes — one fact rather than an arithmetic
       the readers of `npl` each restated. A derived class opens with an inert CONTEXT probe and, where the
       source declares a percent-encode set, a DELIVERY probe beside it; a single-context class opens with its
       written-down vectors and has none. `npl > nprobe` is then exactly "this search has constructed an
       escape", which `npl > 1` only approximated and stopped meaning the moment a second probe existed. */
    int nprobe;
    /* THE PER-CANDIDATE HALF OF THE SURVIVAL PAIR, parallel to `pl` (see push_breakout). `surv_run`/`surv_len`
       is the search's BEST and saturates at a full-length run the moment any one candidate lands intact — the
       ratchet's own consequence — so it cannot say whether the run it is reporting was the inert probe's or a
       breakout's. Those are the two flows whose only difference is these bytes, so telling them apart IS the
       remaining question. Report-only: the WFQ credit stays on the search-level ratchet, worth at most one
       rung, so adding this changes no ordering. */
    int *surv_pl; int svcap;
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
    /* AND WHETHER THE TABLE IS A MEASUREMENT YET, which is not the same question as what it holds. It starts
       permissive, so "every byte arrives" and "no delivery probe has run" have the same contents and opposite
       meanings — the defaulted-field defect exactly: a permissive table read as an observation would report a
       page that decodes its own fragment for one that has never been asked. The report emits the measured set
       only when this says there IS one, and its absence is the positive statement that none was taken. */
    SolveDelivered deliv; int deliv_seen; char **wit; int nwit, witcap;
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
   find-or-CREATE is the wrong primitive for a caller that is merely observing. Opening a search is an EVENT
   (it credits the running flow with a new attacker-source-reaches-sink), so a lookup that created one would
   both fabricate a search with no breakouts and pay a flow for discovering nothing. */
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
    /* THE TWO NEW RUNGS TAKE THE SAME LINE AS THE OTHER FOUR, and the comment above this block is why: the
       array is realloc'd and never zeroed, so a field left out here reads whatever the allocator held. For a
       best-so-far ratchet that is not merely a wrong report — a garbage `surv_len` makes the first real
       observation compare against a maximum nothing ever achieved and the rung never pays at all. */
    e->surv_run = 0; e->surv_len = 0;
    e->surv_pl = NULL; e->svcap = 0;
    /* EVERYTHING DELIVERS UNTIL A RUN SAYS OTHERWISE — the sound-only direction (solve_filter.h): a search
       whose delivery probe has not come back keeps every arm, exactly as a branch whose domain permits both
       outcomes keeps both. The array is realloc'd and never zeroed, so an omission here would read a
       constraint out of whatever the allocator held and decline escapes at random. */
    solve_delivered_all(&e->deliv);
    e->deliv_seen = 0;
    e->wit = NULL; e->nwit = e->witcap = 0;
    e->reinject = NULL;
    e->escaped = 0;
    e->pl = NULL; e->npl = e->plcap = 0; e->seeded = 0; e->nprobe = 0;
    *created = 1;
    flow_credit_emit(1.0);   /* a NEW attacker-source-reaches-sink: value-of-information for the running flow */
    return e;
}

/* ADD A BREAKOUT TO THIS SINK'S SEARCH, deduped by its TEXT. A probe run reaches one sink as often as the page
   writes it (a loop over innerHTML), and two occurrences of the source can land in the same tokenizer state,
   so the same constructed escape arrives more than once — and each duplicate would otherwise be a whole extra
   re-run of the page that can only reproduce a result already had. */
static void push_breakout(Cand *e, const char *payload) {
    DCHECK(e && payload && *payload, "a breakout was queued onto no sink, or with no bytes in it");
    for (int i = 0; i < e->npl; i++) if (!strcmp(e->pl[i], payload)) return;
    if (e->npl >= e->plcap) {
        e->plcap = e->plcap ? e->plcap * 2 : 8;
        e->pl = realloc(e->pl, (size_t)e->plcap * sizeof(char *));
        CHECK(e->pl, "solve: OOM recording a breakout for a sink search");
    }
    e->pl[e->npl] = strdup(payload);
    CHECK(e->pl[e->npl], "solve: OOM recording a breakout for a sink search");
    /* PARALLEL TO `pl` AND GROWN WITH IT, so the two cannot come apart. `surv_pl[i]` is the longest run of
       `pl[i]` ITSELF that has ever been seen at a sink — the per-candidate half of `surv_run`/`surv_len`, which
       saturates at 1.0 the moment ANY candidate lands intact and then cannot say WHICH one did. A derived
       class's `pl[0]` is its inert context probe and `pl[1..]` are the breakouts built from what that probe
       observed, so this array is exactly the statement "the probe's bytes got somewhere and the breakout's did
       not" — the two flows differ in nothing but these bytes, so that is the whole question. */
    if (e->plcap > e->svcap) {
        e->surv_pl = realloc(e->surv_pl, (size_t)e->plcap * sizeof(int));
        CHECK(e->surv_pl, "solve: OOM recording a breakout's own survival");
        e->svcap = e->plcap;
    }
    e->surv_pl[e->npl] = 0;
    e->npl++;
}

/* THE SEARCH LEARNS HOW THE ATTACKER'S BYTES ARRIVE — once, from the value that arrived. A source reaching a
   sink twice reaches it by the same route both times: the root is inherited unchanged through every derivation,
   so two values with the same injection identity cannot have entered by two. Asserted rather than overwritten,
   because if it ever were two the report would state whichever detection ran last. */
static void cand_learn_root(Cand *e, const char *root) {
    DCHECK(e && root, "a sink search was told how its bytes arrive by nothing, or was told nothing");
    if (!e->root) { e->root = strdup(root); CHECK(e->root, "solve: OOM recording a sink's delivery root"); return; }
    DCHECK(!strcmp(e->root, root),
           "one sink search has been handed two different delivery ROOTS for one injection identity — the root "
           "is inherited unchanged through every derivation, so two values spelling the same source cannot have "
           "entered the program by two different components, and the envelope would report whichever detection "
           "ran last");
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
    if (!created) return;
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
    e->reinject = decide_freeze_path();
    sc = sink_class(sink);
    if (sc->vectors) { for (int c = 0; sc->vectors[c]; c++) push_breakout(e, sc->vectors[c]); }
    else {
        /* THE PROBES ARE THE LEADING ENTRIES AND THE COUNT IS TAKEN HERE, at the one moment nothing else has
           been pushed — see `nprobe`. */
        push_breakout(e, derive_probe(sc->derive));
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
                push_breakout(e, bp);
                free(bp);
            }
        }
        e->nprobe = e->npl;
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
 * a cold-resumed entry (solve_resume_candidate) holds no path and seeds nothing through this file, because its
 * candidates come back as FLOWS rather than as payloads.
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
    push_breakout(e, breakout);
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
   CONSTRUCTED and the search never received leaves `npl == nprobe`, which the report states as "this search has
   built no escape" — the state a reader takes to mean the source cannot carry an exit from the state its bytes
   are in. queue_derived has exactly one door that drops (a search closed by a fire between the derivation and
   the push), and a search that fired holds the breakout that fired it, so the implication holds with no
   exception written for it. */
static void derive_from_witness(Cand *e) {
    int derive, built = 0;

    DCHECK(e != NULL && e->nwit > 0,
           "a derivation was asked to run on a search that holds no witness — the witness is the string the "
           "context probe's own run handed this sink, so without one there is no observation to read a state "
           "off and the re-derivation would be a static shape of the expression");
    derive = sink_class(e->sink)->derive;
    if (derive != SINK_DERIVE_HTML && derive != SINK_DERIVE_JS)
        DFAIL("a sink class stored a context witness and declares no derivation to read it with — a class "
              "whose breakouts are written down never stores one, so this is a class whose derivation column "
              "was set without a parser being routed for it");
    for (int i = 0; i < e->nwit; i++) {
        if (derive == SINK_DERIVE_HTML) built += solve_html_breakouts(e->wit[i], &e->deliv, queue_derived, e);
        else                            built += solve_js_breakouts(e->wit[i], &e->deliv, queue_derived, e);
    }
    DCHECK(built == 0 || e->npl > e->nprobe,
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
    DCHECK(e->npl > e->nprobe,
           "a sink recorded a BREAKOUT arriving while its search holds nothing but its own probes — a derived "
           "class's breakout exists only because a probe run returned one, and a single-context class's "
           "vectors are not probes at all (nprobe is 0 for it), so these bytes were not built by this search");
    /* §@S's SECOND FITNESS RUNG, PAID INTO THE WFQ — "the search is DISTANCE-DIRECTED (a fitness of
       {filter-survived, sink-reached, context-escaped, handler-fires} the WFQ reads)". flow_weight reads `val`
       and nothing else, and a candidate flow records no endpoints by design (endpoint_suppress), so until this
       line the ONLY thing that could ever raise a candidate's reward was record_sink — the LAST rung, a
       fire-verified PoC. A candidate that carried the attacker's bytes all the way to the sink and did not
       break out of it was worth exactly as much to the scheduler as one that had not started, so the near-miss
       §@S says to mutate toward the gap was outranked by every arm of the exploration tree and never ran again.
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
       there is one rung, once, and the COMPARATOR still separates the two candidates — filter_survived writes
       each flow's own surviving fraction, so the spelling that arrives intact stands at 1.0 and the one that
       arrives as an attribute name stands at its marker's four bytes. */
    if (solve_delivered_ok(&e->deliv, f->cand_payload) && e->reach_credited == 0) {
        e->reach_credited = 1;
        flow_credit_emit(1.0);
    }
    e->reached++;
}

/* §@S's FIRST FITNESS RUNG — "filter-survived" — AND THE ONLY ONE THAT IS OBSERVED INSIDE THE RUNWAY.
   The other three all report at or past the sink of the candidate's OWN class, so a flow that had not got
   there yet was worth nothing whatever it had done. This one is asked of EVERY string that reaches ANY
   code-execution sink during a candidate run, before the class partition, because that is what the question
   is: not "did this breakout arrive at its sink" but "how much of what the page was given is still alive".
   A candidate for the eval sink whose bytes turn up at a markup write has demonstrably survived the page's own
   filter and travelled through its code — §@S(2)'s "moved" — and until this line that observation was
   discarded by `candidate_search` returning NULL.
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
       improvement test. A zero run is an observation with a value of zero and flow_set_distance discards it as
       a non-improvement; a run that ties the search's best is no news to the ledger and is the whole news to
       the comparator, because it says THIS candidate is where the best one got. */
    flow_set_distance(f, (double)o.run / (double)o.len);
    if (o.run == 0) return;                  /* none of this candidate is in this string: an OBSERVATION */
    /* WHICH CANDIDATE THIS IS, recorded before the search-level ratchet because the ratchet is what erases the
       distinction. A NEGATIVE index is a POSITIVE statement and not a miss: solve_resume_candidate raises
       `tried` for a cold-resumed candidate whose payload rides the resumed FLOW rather than this session's
       record, so its bytes are legitimately not in `pl` (solve.h says the same thing about `payloads` being
       empty beside a non-zero `tried`). The search-level pair still records it, so nothing is lost — only the
       per-candidate column, which this session has no row for. */
    {
        int idx = -1;
        for (int i = 0; i < e->npl; i++) if (!strcmp(e->pl[i], f->cand_payload)) { idx = i; break; }
        if (idx >= 0) {
            DCHECK(o.len == (int)strlen(e->pl[idx]),
                   "a candidate's payload matched its search's record by text and disagrees with it by length — "
                   "the two are the same bytes by construction, so the per-candidate survival column is about "
                   "to be scaled by a denominator that is not this candidate's");
            if (o.run > e->surv_pl[idx]) e->surv_pl[idx] = o.run;
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
   ONCE PER SEARCH AT THE 0->1 CROSSING — the shape breakout_arrived uses, and for its reason: this is a
   boolean fact about the search, so it has exactly one crossing and repeating the credit would pay twice for
   one observation. */
static void escape_reached(Cand *e) {
    DCHECK(e != NULL, "a context escape was recorded against no search — the caller resolved one to record the "
                      "arrival that must precede it");
    DCHECK(e->reached > 0,
           "a breakout was observed in an EXECUTABLE position at a sink its own bytes have not been recorded "
           "as ARRIVING at — every escape site runs downstream of breakout_arrived on the same string, so an "
           "escape with no arrival behind it means the two are being asked about different strings");
    if (e->escaped == 0) flow_credit_emit(1.0);
    e->escaped++;
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

/* HTML firing oracle: re-parse the sink output with the REAL Lexbor parser and FIRE the auto-firing event
   handlers (svg/body onload, img/script onerror) by eval'ing their JS — X9 fires iff a breakout placed
   executable JS in an auto-firing position. innerHTML does NOT run <script>, so those never fire (correct). */
/* THE TREE'S DEPTH IS THE CANDIDATE'S DATA — a breakout that nests `<div>` a million times is exactly the kind
   of input this walk exists to run — so descending by C frame made the oracle's own depth attacker-controlled.
   Lexbor's nodes carry `parent`, so the traversal needs no stack at all: descend to first_child, else take
   `next`, else climb until a `next` exists, never above the level the walk started at. */
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
static int html_fire_walk(Cand *e, lxb_dom_node_t *node) {
    lxb_dom_node_t *top = node->parent;   /* the level the walk must not climb above */
    lxb_dom_node_t *n = node;
    int at_exec = 0;

    while (n) {
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
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) {
            n = n->parent;
            if (n == top) n = NULL;
        }
        if (n) n = n->next;
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

/* A PARKED CANDIDATE COMING BACK — see solve.h for why the re-binding and the bookkeeping are ONE call. */
const char *solve_resume_candidate(const char *src, const char *root, const char *sink_name) {
    int i, cls = -1;

    /* THE ROOT IS PART OF THE IDENTITY THAT CROSSES, and it was the part that did not. A resumed candidate
       opens its search here rather than at a detection — a verifying flow does not detect — so with no root
       in the record the entry stood at NULL and every emit of it hit emit_delivery's assert: in dev the whole
       report aborted, and in release the envelope rendered the silence that MEANS "no component carries these
       bytes" over a payload whose delivery the ended session knew exactly. */
    DCHECK(src && *src && root && *root && sink_name && *sink_name,
           "a parked @S candidate was rebuilt without a source, without a delivery root or without a sink "
           "class — its identity IS the substitution it carries and the route those bytes take to the victim, "
           "so any one missing makes it an exploration flow wearing a payload, or a payload nothing delivers");
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
       five times — exactly the number of candidate runs that sink's search has had, and exactly what the
       parked-search entry reports. */
    {
        int created = 0;
        Cand *e = sink_search(src, cls, &created);
        /* AND THE ROOT GOES ON BEFORE THE ENTRY IS VISIBLE TO ANY READER, which is what makes this the second
           of the search's two doors rather than a hole beside the first. On the fifth resumed candidate of one
           sink this is cand_learn_root's equality assert over what the fourth wrote, which is the only thing
           that can say a park document's records still agree with each other. */
        cand_learn_root(e, root);
        e->tried++;
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
               no count, no age, no retry limit and no seen-set here: the ONE question is whether a positive
               observation of this search's own says these bytes do not arrive. The table narrows only on
               evidence (a token that never showed up says nothing about its byte, so uncertainty keeps the
               arm) and it never widens, so a withdrawal is permanent and the cursor stays a cursor — nothing
               is re-examined, nothing is seeded twice, and a spelling the tightened table still permits is
               seeded exactly as before.
               THE ENTRY STAYS IN `pl`, AND THAT IS THE REPORT'S HALF OF THE SAME FACT. A search that
               CONSTRUCTED an escape and withdrew it is not a search that constructed none — `probes ==
               payloads` means the second, and compacting the list would say it. `withdrawn` (solve_json_array)
               is what tells the two apart per entry, and it is also what keeps `tried` and `payloads`
               readable together now that they legitimately differ.
               PROBES ARE NOT ESCAPES AND ARE NEVER WITHDRAWN. solve_filter.c's own header says the question is
               asked of "a constructed escape"; the DELIVERY probe is built OUT OF the very bytes in question,
               so a table it narrowed would contradict the instrument that measured it, and the CONTEXT probe
               is ASCII alphanumeric precisely so it cannot change the parse it reads. Both are measurements,
               not attacks, and `nprobe` is where that line already is. */
            if (e->seeded >= e->nprobe && !solve_delivered_ok(&e->deliv, e->pl[e->seeded])) continue;
            f = flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* a candidate session runs from the baseline */
            f->cand_src     = strdup(e->src);
            f->cand_payload = strdup(e->pl[e->seeded]);
            f->cand_sink    = sink_name(e->sink);
            CHECK(f->cand_src && f->cand_payload, "solve: OOM seeding a candidate flow");
            /* THE OTHER SIDE OF THE WITHDRAWAL, ASSERTED WHERE THE COST IS TAKEN RATHER THAN WHERE THE
               DECISION IS MADE. This line is the moment a payload becomes a document re-run, so it is the one
               place at which "a contradicted candidate is still queued" stops being a list state and starts
               costing a traversal — and the push-time check being the ONLY check is exactly what let that
               happen once. A route into flow creation that does not pass the skip above crashes here instead
               of quietly spending the run and the rung. */
            DCHECK(e->seeded < e->nprobe || solve_delivered_ok(&e->deliv, f->cand_payload),
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
        if (e) e->turns++;
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

    if (enc) { json_buf_puts(b, ",\"sourceEncodes\":"); json_buf_str(b, enc); }
    /* IMMEDIATELY AFTER THE DECLARATION IT IS A SUBSET OF, because the pair is one statement and a reader has
       to be able to hold the two against each other. */
    if (delivers) {
        DCHECK(enc != NULL,
               "a measured delivery set is being emitted for a source that declares no percent-encode set — "
               "the measured set is the subset of the DECLARED one a run saw arrive, so a measurement with no "
               "declaration behind it is a subset of nothing");
        json_buf_puts(b, ",\"sourceDelivers\":"); json_buf_str(b, delivers);
    }
    if (concolic_source_delivery(root, &kind, &prefix) && kind) {
        json_buf_puts(b, ",\"delivery\":"); json_buf_str(b, kind);
        if (prefix) {
            char p[2] = { prefix, 0 };
            json_buf_puts(b, ",\"deliveryPrefix\":"); json_buf_str(b, p);
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
    json_buf_puts(&b, "[");
    for (int i = 0; i < g_sinks_n; i++) {
        const SinkClass *sc = sink_class(g_sinks[i].cls);
        const PolicyContainer *pc = document_policy(ctx);

        if (n++) json_buf_puts(&b, ",");
        json_buf_puts(&b, "{\"sink\":"); json_buf_str(&b, sc->name);
        json_buf_puts(&b, ",\"source\":"); json_buf_str(&b, g_sinks[i].source);
        json_buf_puts(&b, ",\"poc\":"); json_buf_str(&b, g_sinks[i].poc);
        /* §S(d): EVERY PoC CARRIES ITS REPRODUCTION ENVELOPE. What makes it RUN is the first thing a reader
           needs and the last thing this record carried — the vector was decided one line below, for the CSP
           question, and thrown away. It comes off the sink's own fire oracle (see the table). */
        json_buf_puts(&b, ",\"firesOn\":"); json_buf_str(&b, sc->fires_on);
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
                json_buf_puts(&b, ",\"cspBlocks\":");
                json_buf_str(&b, policy_container_csp(pc));
            }
        }
        /* AND THE SAME RULE ONE ALGORITHM EARLIER. Under `require-trusted-types-for 'script'` an innerHTML
           assignment THROWS before the markup is ever parsed, so the model's breakout is real and the write
           that carries it never happens on the real page. trusted_types.c has answered this question all
           along and nothing asked it. The emitted value is the SINK GROUP the CSP names, because that is what
           the directive is written in terms of and what a reader has to add a policy for. */
        if (sc->tt >= 0 && trusted_types_required(ctx, (TrustedTypeKind)sc->tt)) {
            json_buf_puts(&b, ",\"trustedTypes\":");
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
            json_buf_puts(&b, ",\"searched\":");
            snprintf(t, sizeof t, "%d", tw->tried); json_buf_puts(&b, t);
            /* AND THE MEASURED CONSTRAINT THE PoC WAS BUILT UNDER, from the same search. On a FIRED entry it
               is what says which bytes the exploit is allowed to contain, so a reader reproducing it by hand
               knows which of them the browser would have eaten — a fact the payload alone does not carry. */
            emit_delivery(&b, g_sinks[i].root, cand_delivers(tw, dv, sizeof dv));
        }
        json_buf_puts(&b, "}");
    }
    for (int i = 0; i < g_pending_n; i++) {
        char t[32];
        if (solved(g_pending[i].sink, g_pending[i].src)) continue;
        if (n++) json_buf_puts(&b, ",");
        json_buf_puts(&b, "{\"sink\":"); json_buf_str(&b, sink_name(g_pending[i].sink));
        json_buf_puts(&b, ",\"source\":"); json_buf_str(&b, g_pending[i].src);
        json_buf_puts(&b, ",\"search\":\"parked\",\"tried\":");
        snprintf(t, sizeof t, "%d", g_pending[i].tried); json_buf_puts(&b, t);
        /* …AND HOW MANY OF THOSE RUNS GOT HERE, which is the half `tried` cannot state (see the field). The
           two together are the only thing that tells a document nobody has explored far enough apart from a
           breakout that arrived and did not work — `reached:0` is the first, anything else is the second. */
        json_buf_puts(&b, ",\"reached\":");
        snprintf(t, sizeof t, "%d", g_pending[i].reached); json_buf_puts(&b, t);
        /* …AND HOW MANY TURNS THE SCHEDULER HAS GIVEN IT, which is what makes `reached:0` readable: with
           `turns:0` this search's candidates have never once held the thread, and with `turns:N` they have run
           and have not got as far as the sink. One is a WFQ question and the other is a distance question. */
        json_buf_puts(&b, ",\"turns\":");
        snprintf(t, sizeof t, "%d", g_pending[i].turns); json_buf_puts(&b, t);
        /* …AND THE TWO MIDDLE RUNGS, WHICH IS WHAT SPLITS `reached:0` AND `reached:N` INTO THE FOUR STATES THEY
           REALLY ARE. `survived`/`survivedOf` is the FURTHEST any candidate of this search has got its own
           bytes through the page's own transforms to ANY sink, so `turns:900,reached:0,survived:0` is a
           document nobody has explored far enough and `turns:900,reached:0,survived:11,survivedOf:14` is a
           FILTER eating the candidate eleven-fourteenths of the way in — the same report before, opposite work.
           `escaped` is how many arrivals reached an EXECUTABLE position, which `reached` could not say and
           which `fires` only approximates: `fires` counts every auto-firing handler in the parse INCLUDING the
           page's own markup, so a card reading it alone stated "none reached an executable position" as a fact
           about the payload on evidence that was partly about the template around it.
           BOTH ARE UNCONDITIONAL, and 0 is a real value each of them must be able to say (nothing of any
           candidate has been seen at any sink; nothing has got out of its context). `survivedOf:0` beside
           `survived:0` is the same statement said once — no observation has been recorded — and not a length
           this file failed to write. */
        json_buf_puts(&b, ",\"survived\":");
        snprintf(t, sizeof t, "%d", g_pending[i].surv_run); json_buf_puts(&b, t);
        json_buf_puts(&b, ",\"survivedOf\":");
        snprintf(t, sizeof t, "%d", g_pending[i].surv_len); json_buf_puts(&b, t);
        json_buf_puts(&b, ",\"escaped\":");
        snprintf(t, sizeof t, "%d", g_pending[i].escaped); json_buf_puts(&b, t);
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
            json_buf_puts(&b, ",\"fires\":");
            snprintf(t, sizeof t, "%d", g_pending[i].fires); json_buf_puts(&b, t);
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
            json_buf_puts(&b, ",\"witnessed\":");
            snprintf(t, sizeof t, "%d", g_pending[i].nwit); json_buf_puts(&b, t);
        }
        /* HOW MANY OF `payloads`' LEADING ENTRIES ARE PROBES — the producer fact that splits `reached:0` one
           more time, and the one state of this search the report could not say at all. `npl > nprobe` is
           stated at the field's declaration as exactly "this search has constructed an escape", and it is
           asserted at queue_derived; it was never emitted, so a reader had the two halves of the question and
           not the question. The consumer must not re-derive it: the probe is told apart by carrying no marker,
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
        json_buf_puts(&b, ",\"probes\":");
        snprintf(t, sizeof t, "%d", g_pending[i].nprobe); json_buf_puts(&b, t);
        json_buf_puts(&b, ",\"payloads\":[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_puts(&b, ",");
            json_buf_str(&b, g_pending[i].pl[c]);
        }
        json_buf_puts(&b, "]");
        /* …AND HOW FAR EACH OF THOSE PAYLOADS GOT, one entry per `payloads` entry and in the same order, so a
           reader lines the two up by index rather than by guessing. It is the column `survived` cannot have:
           the ratchet saturates the moment ANY candidate lands intact, so `survived:16 survivedOf:16` beside
           `reached:0` says a full-length run of SOMETHING arrived and nothing about WHICH — and for a derived
           class the two candidates differ in nothing except these bytes, so that is the entire question.
           `[14,0]` says the inert probe's bytes reached a sink and the breakout built from them never did;
           `[14,9]` says the breakout travelled too and something after arrival is the problem. Those take
           opposite work and `survived` alone reports them identically. */
        json_buf_puts(&b, ",\"survivedBy\":[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_puts(&b, ",");
            snprintf(t, sizeof t, "%d", g_pending[i].surv_pl[c]); json_buf_puts(&b, t);
        }
        json_buf_puts(&b, "]");
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
        json_buf_puts(&b, ",\"withdrawn\":[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_puts(&b, ",");
            json_buf_puts(&b, (c >= g_pending[i].nprobe &&
                               !solve_delivered_ok(&g_pending[i].deliv, g_pending[i].pl[c])) ? "1" : "0");
        }
        json_buf_puts(&b, "]");
        /* The parked entry carries the DECLARATION, not the envelope: a search that has not solved has no
           vector to state and no PoC to reproduce, so `firesOn`/`cspBlocks`/`trustedTypes` would be claims
           about a PoC that does not exist. What it does carry is the whole source declaration — the bytes a
           candidate must survive AND how the attacker would have to reach the victim if one ever fires. */
        {
            char dv[64];
            emit_delivery(&b, g_pending[i].root, cand_delivers(&g_pending[i], dv, sizeof dv));
        }
        json_buf_puts(&b, "}");
    }
    json_buf_puts(&b, "]");
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
        for (int c = 0; c < g_pending[i].npl; c++) free(g_pending[i].pl[c]);
        free(g_pending[i].pl);
        free(g_pending[i].surv_pl);   /* grown with `pl` (push_breakout), so freed beside it */
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
