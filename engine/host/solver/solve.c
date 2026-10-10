/* @S solver (see solve.h). For each sink an attacker source reaches, derive a breakout from the sink's real lexical
   context, inject it at the source, re-run the page's own code as a candidate flow, and prove it by firing. */
#include "solver/solve.h"
#include "solver/solve_html.h"
#include "solver/solve_js.h"
#include "solver/solve_filter.h"
#include "core/json_buf.h"
#include "core/frame/policy_container.h"
#include "core/html/trusted_types.h"
#include "core/dom/document.h"
#include "core/dom/node.h"             /* node_next_in, the pre-order successor bounded to a subtree */
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
#include "core/dom/node_interface.h"   /* dom_document_create, where a Document is made */
#include "core/html/html_parse.h"      /* html_parse_document, which owns the tokens it produces */

enum { SINK_EVAL = 0, SINK_HTML = 1, SINK_URL = 2 };   /* sink classes, each with its own breakouts and fire oracle */

/* Candidate flows seeded this session, fresh or resumed. Each re-runs the page, so this count times the page's
   cost is most of what @S spends; it is reported beside the switch count so a run that slowed can be read as
   more searches or as bigger ones. */
static int g_cands_seeded;
int solve_candidate_count(void) { return g_cands_seeded; }
/* The arrival census, which makes an empty @S surface readable (see detect_sink). `reached` counts sink
   executions, `tainted` those that carried attacker input, and `suppressed` those whose search the
   unforgeable-principal rule declined to open. Only the triple is a reading, so one call returns all three. */
static long g_sink_reached, g_sink_tainted, g_sink_suppressed;
void solve_arrival_census(long *reached, long *tainted, long *suppressed) {
    DCHECK(reached && tainted && suppressed,
           "the @S arrival census was asked for fewer than its three numbers — each is uninterpretable alone "
           "(see the counters' own declaration), so a caller taking one of them is about to report a state it "
           "cannot distinguish from its opposite");
    *reached = g_sink_reached; *tainted = g_sink_tainted; *suppressed = g_sink_suppressed;
}

/* The running flow's candidate mode, read through the running flow so a preemption cannot cross it. A NULL flow
   (baseline setup) is not verifying. */
static int  is_verifying(void)   { Flow *f = flow_running(); return f && f->cand_verifying; }

/* One candidate of a sink's search: its bytes, what kind of thing they are, and how much of them has reached a
   sink. The kind belongs to the entry, written by the pusher, and never to its position in the list: the seeder
   must not withdraw a probe, the arrival assert must know whether an escape was received, and the report splits
   probes from attacks. One array of one record keeps the members from coming apart at a push, free or clone. */
#define CAND_PROBE  1   /* an instrument — inert by construction, never an attack, never withdrawn */
#define CAND_ESCAPE 2   /* an attack — derived from a witness, or a single-context class's written-down vector */
typedef struct {
    char *bytes;   /* owned */
    int   kind;    /* CAND_PROBE | CAND_ESCAPE — see above */
    /* The longest run of these bytes seen at any sink. The search-level `surv_run` saturates as soon as any
       candidate lands intact, so only this column says whether that run was the probe's or a breakout's.
       Report-only: no WFQ ordering reads it. */
    int   surv;
} CandPayload;

/* One principal and the predicates the replayed path made of it (see Cand.pg). The search owns `src` and the
   rows because both sources are borrowed: the registry name lives while its component holds its claim, and the
   rows concolic_strpred_read returns live until the running flow's constraint next grows. What a row owns is
   concolic.c's to say (concolic_pred_copy / concolic_pred_release); this file owns the array and the name. */
typedef struct { char *src; ConcolicPred *pred; int npred; } PrincipalGate;

/* The verdict on what a search's frozen path demanded of an attacker's principal. It is a separate fact from the
   demands in `pg`: the rows are the evidence, this is the decision, and a consumer branches on this.
   PG_UNEXAMINED is 0, so the Cand compound literal in sink_search leaves a never-frozen path unexamined without
   naming the member. PG_NONE is never spelled by an absence: a trusted-zone reader meeting an artifact built
   before this field existed must read "unstated" and refuse, rather than deliver with an opaque identity to a
   victim that gates on origin and then read the miss as an engine-fidelity divergence. */
typedef enum {
    PG_UNEXAMINED = 0,   /* no path was ever frozen for this search, so nothing has been asked */
    PG_NONE,             /* examined: the frozen path demanded nothing of any declared principal */
    PG_FORGEABLE,        /* examined: it demanded a shape an attacker can hold — `pg` carries which */
    PG_UNFORGEABLE       /* examined: it pinned one, which no cross-document attacker can meet */
} PrincipalDemand;

/* One sink's search, keyed by (injection identity, sink class). Its members are three accounting units. Report
   counters (`reached`, `escaped`, `substituted`, `sink_strings`, `ends`, the runway fields) move no WFQ
   ordering. The ledger (`reach_credited`, `escape_credited` and the `surv_run` ratchet) is paid once per
   search per distance. The comparator that orders a search's live candidates lives on the Flow (flow.h's
   `cand_replay`, `cand_surv`, `cand_rung`), because a ledger pays nobody twice for a rung and so cannot order
   candidates standing at different distances. Every member whose blank is not 0 is named at sink_search. */
typedef struct {
    /* `src` is the injection identity a candidate is substituted at. `root` is the delivery provenance, read off
       the value at detection (concolic_root_c) and never re-derived from `src`: the registry matches declared
       sources exactly, and a derived identity such as `{location.hash}.slice()` matches none.
       `tried` counts candidate runs seeded or resumed, the context probe included (it re-runs the page like any
       breakout). It is raised at seeding, so it says nothing about how far a run got.
       `reached` counts arrivals of this search's breakouts at its own sink, never of its probes: a probe cannot
       fire, and a derived breakout exists only because the probe returned one, so `tried >= 2` already states
       the probe's arrival. `reached:0` beside `tried:N` says no breakout has re-executed as far as the sink.
       `turns` counts switch-ins of this search's candidate flows (solve_flow_begin), which tells a search never
       scheduled from one whose flows ran short of the sink. `fires` counts programs a fire oracle queued,
       including the page's own handlers in the parsed markup (html_fire_walk). */
    char *src; char *root; int sink; int tried; int reached; int turns; int fires;
    /* How many times this search's bytes entered the page's program, counted where the substitution is
       performed (solve_observe_substitution, from concolic.c). It is the bottom rung of the report, so
       `substituted:0` beside `turns:N` says no run reached its own source read: a question about the path in
       front of the source or about the schedule, which `ends` tells apart. It counts substitutions, not
       candidates, since a page reading its source in a loop delivers many times per run; the zero is what is
       load-bearing. A report counter: no rung and no credit moves at the write. */
    int substituted;
    /* How many candidate flows of this search have finished (solve_flow_end): a lifetime count for this
       session, which separates a candidate the path turned away from one still on its way. It counts flow
       finishes, not candidates. A candidate session is a tree of flows (engine.c's fork copies the candidate
       identity to every arm), so the arms of one seed each raise it while `tried` rose once, and `ends` may
       exceed `tried`; the only assertable relation is `tried > 0` at the raise. Per session like `tried`:
       nothing persists it and the resume door does not raise it. See the `candEnds` emit for its residual. */
    int ends;
    /* The furthest any candidate of this search has replayed its own recorded decision path, in thousandths of
       Flow.cand_replay (flow_observe_replay writes it per consumed arm; flow_observe_rung pins it to 1.0 at the
       delivery). Integer thousandths so json_buf never prints a locale-dependent float; the unit is in the key,
       `runwayPerMille`. A best-so-far like `surv_run`, because the record outlives its flows; a report counter.
       It is a fraction of decision arms, not of statements: a short path saturates it at 1000 while the source
       read is still far off. Its 0 has three readings, split by the members below: no recorded path at all
       (`reinject_len` 0), arms offered and none consumed, and arms consumed but rounded away (`replay_arms`).
       It is kept beside that pair because it carries flow_observe_rung's delivery pin, a policy, while the
       pair is only ever an observation; below the delivery the two agree. */
    int replay_pm;
    /* How many arms the frozen re-injection path holds, emitted as `runwayArms`. The path is frozen once per
       search (cand_learn_path) and every candidate starts on it at cursor 0, so its length is a property of the
       search. `runwayArms:0` means the detecting flow decided no branch, and a zero runway is then a tautology
       about the detection; `runwayArms:N` beside `runwayPerMille:0` means N arms were offered and none was
       consumed. It is not the runway fraction's denominator, which is `replay_of`. It is stored at the freeze
       because record_sink releases `reinject` at the fire, so a length read at emission would be 0 for every
       search that succeeded. A size, never a distance. extension/lib/popup-security.js reads it, with
       `replay_arms` and `replay_of`, and lib/store-record.js's currency predicate requires all three. */
    int reinject_len;
    /* The runway position as its two halves, emitted as `runwayWalked`/`runwayOf`, so a best of 3/8000 is not
       rounded to 0 as it is in `replay_pm`. Ratcheted on itself by cross-multiplication (observe_runway), in
       integers. `replay_of` is dec_total() at that sample, not `reinject_len`: for an arm forked off a
       candidate it includes the slots that fork appended. `replay_of == 0` means no reading was taken, which
       flow.h asserts as a biconditional with the fraction. */
    long replay_arms, replay_of;
    /* How many strings a code-execution sink was handed while this search's substitution was live: the
       observation count `surv_run` is the best of. It separates `surv_run:0` with no sink run from S sinks run
       with none of the payload in them, which take opposite work. Class-independent and attributed to the
       running flow's search, like the survival fraction (filter_survived). It counts strings, not arrivals;
       `reached` counts a breakout at its own sink. */
    int sink_strings;
    /* The ledger's latch for the arrival rung, separate from the report counter `reached`. Every arrival is
       counted, including one by a spelling the delivery table has since contradicted, but only a deliverable
       one pays the crossing: the rung is a distance to firing, and a contradicted candidate made no progress
       toward one. */
    int reach_credited;
    /* The best survival fraction any candidate of this search has reached, held as its two halves so the report
       can say "11 of 14 bytes". A ratchet: each improvement pays exactly the fraction it added, so the rung is
       worth at most 1.0 over the search's life and needs no minimum run length. 0/0 is no observation yet. */
    int surv_run, surv_len;
    /* Where that run is: `surv_at` is its offset into the candidate (so the rest is the segment that died) and
       `surv_out` its offset in the string the sink was handed. A run at offset 0 is a payload whose tail the page
       cut, a later offset one whose head it ate, and those take opposite mutations. Written only in the
       ratchet's improvement branch, so all four describe one observation. -1 is no observation, as in
       FilterObs, which is why the report omits them rather than emitting 0. */
    int surv_at, surv_out;
    /* Report counter: arrivals at an executable position (escape_reached), contradicted spellings included. It
       separates bytes that arrived from bytes that got out. */
    int escaped;
    /* The ledger's latch for the escape rung, for `reach_credited`'s reason. A latch read off `escaped` would
       let a contradicted escape spend the crossing the spelling that can fire needs. The count stays outside
       the deliverability gate so the report still says a contradicted spelling reached an executable
       position. */
    int escape_credited;
    /* This search's candidates in push order, and `seeded`, the cursor past which they are not yet flows. A
       derived sink learns its breakouts after detection (a probe run reads them off the sink's own parse), so
       the list grows after seeding; each drain seeds everything past the cursor, so a late breakout is picked
       up and none is seeded twice. Held as text, not as a class-table index, because a parked candidate
       carries its payload as bytes (cold.c) to a build whose tables may differ. */
    CandPayload *pl; int npl, plcap, seeded;
    /* How many of this search's runs came back out of the cold tier (solve_resume_candidate): the term of
       solve.h's arithmetic for `tried` that has no row in `pl`, because a resumed payload rides the flow.
       Without it `tried:6` beside an empty `payloads` cannot be told from a producer that dropped a field, and
       `reached` cannot be read beside a list holding only probes. */
    int resumed;
    /* How many parked candidates came back and were refused (solve_resume_candidate), because this build's
       carrier declaration (cand_learn_root) contradicts their payload. A withdrawn record never runs, so neither
       `tried` nor `resumed` moves; without this, "never parked", "parked and ran" and "parked and refused"
       collapse into `tried:0,resumed:0`. A withdrawal creates the slot and raises nothing else, so add_pending
       reads this to account for an entry standing at `tried:0`. */
    int resumed_withdrawn;
    /* Has detection opened this search (add_pending). This is not the same as having created the slot: a cold
       resume runs at engine init, so in a resuming session it creates the slot first, and only detection,
       standing at the sink with the value that arrived, can take the probe and the re-injection path. */
    int opened;
    /* `deliv` is the delivery table: which bytes of a candidate reach this sink. It is measured
       (observe_delivery) rather than read off the source's percent-encode declaration, because a page that runs
       decodeURIComponent over its own fragment receives the `<` the browser encoded; the declaration only
       decides which bytes the delivery probe asks about. The root's carrier refusals narrow it as well
       (cand_learn_root). It starts permissive and only narrows.
       `deliv_seen` is raised only by a run that found a probe token, so the report emits a measured set only
       when one was taken. `deliv_runs` counts delivery-probe arrivals at a sink whether or not a token
       survived: a probe the page destroyed is evidence, not a probe that never ran.
       `wit` holds the distinct strings the context probe's runs handed this sink, deduped by text. A narrowed
       table re-derives from them (derive_from_witness), which mutates a near miss using the observation
       already taken rather than a second probe. It is a list because a page may write one source into a sink
       many times, each write with its own contexts. */
    SolveDelivered deliv; int deliv_seen; int deliv_runs; char **wit; int nwit, witcap;
    /* The search's re-injection point: the decision state of the flow standing at this sink, frozen once
       (cand_learn_path) so every candidate replays that path instead of re-forking the document's whole gate
       tree; a pathless candidate behind K gates re-forks 2^K flows. A flow is replay(baseline, decision
       vector), so a candidate standing on a vector it did not run is what cold_resume already builds. It is
       sound under a different payload because the substituted source is concrete in a candidate run: branches
       on it run the real predicate, and the vector replays only the arms the payload does not decide.
       The blob holds a reference on the frozen segment. record_sink releases it at the fire, which is what
       closes the search (search_seeds), and solve_free releases what remains. */
    void *reinject;
    /* What that frozen path demanded of an attacker's principal, captured beside the path
       (cand_learn_principal_gates) because every candidate replays that one path. Not a union across
       sightings: an identity satisfying a gate on a path no candidate replays gives a PoC that fails for a
       reason that is not an engine divergence. endpoint.c intersects rows across sightings because @H states
       what every path obeyed; @S needs what one replayed path demands. Each principal's rows are a flat
       conjunction. `pg_demand` is the verdict a consumer branches on and is never derived from `npg` here; see
       PrincipalDemand. */
    PrincipalGate *pg; int npg; PrincipalDemand pg_demand;
} Cand;
static Cand *g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;

/* The sink class table (SINKS), defined below its candidate sets. Every fact a report states about a sink is a
   column of its row, so these are how the rest of the file reaches one. */
typedef struct SinkClass SinkClass;
static const SinkClass *sink_class(int sink);
static const char      *sink_name(int sink);
static int              sink_class_of_name(const char *name);
/* Declared here for the fire marker, which solve_init installs above the store's definition. */
static void record_sink(int cls, const char *source, const char *poc);

/* A fire-verified PoC. The sink is held as its class, because every fact the reproduction envelope states (CSP,
   Trusted Types, what makes the breakout run) is a column of the class row. */
typedef struct { int cls; char *source; char *root; char *poc; } Finding;   /* verified PoCs only */
static Finding *g_sinks = NULL; static int g_sinks_n = 0, g_sinks_cap = 0;

/* The fire marker X9. Only firing proves a PoC, so the finding is recorded the instant the marker runs and not
   at flow completion: a flow owes nobody completion, cold.c drops `cand_fired` on a park, and flow_remove frees
   a sold flow's substitution without passing solve_flow_end. A call outside a candidate flow is the page's own,
   since X9 is a global the bundle can reach, and proves nothing. */
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

/* Run a fired breakout as more code of the running flow. The sunk code is the page's and may loop, await or
   recurse, so it is queued as a program of this flow (engine_queue_candidate) and the one BFS runs it,
   preemptible and parkable. The position is part of the sink's firing semantics; both callers append, because
   an event handler and a javascript: URL evaluation each run from a task. The payload crosses as (src, len),
   so a U+0000 in it is fired whole; the queue copies it into the body it makes (solver/dyn_body.h). */
static void fire_js(const char *src, size_t len, DynPos pos) {
    engine_queue_candidate(src, len, pos);
}

/* The written-down breakouts of a single-context sink. Only the URL sink qualifies: navigating executes the
   `javascript:` scheme and nothing else does, so there is no context to derive. Markup and eval sinks derive
   theirs instead, from the real parse of the sink's own output (solve_html.c, §13.2.5) and from the eval
   argument's lexical state (solve_js.c, ECMAScript §12), and each crashes on a state it cannot name. */
static const char *CANDS_URL[] = {
    "javascript:X9()",     /* the vector is the javascript: scheme itself, the class's one fixed context */
    "javascript:X9()//",
    NULL
};

/* Where a sink class gets its breakouts. These are two different algorithms, not a fallback: a class declares
   exactly one, written-down vectors or a derivation, and solve_init asserts it. */
enum { SINK_DERIVE_NONE = 0, SINK_DERIVE_HTML, SINK_DERIVE_JS };
/* The sink classes: one row per lexical context the solver breaks out of, holding every fact about the sink
   rather than about a run, which together are the sink's fire oracle. `fires_on` is read off that oracle:
     - eval evaluates its own argument. 19.2.1 and 20.2.1.1.1 announce the source to this file and then compile
       and run it on the flow's own tramp chain, in the scope and strictness the spec gives it;
     - html_fire_walk runs only the auto-firing `onload`/`onerror` handlers of markup it has just parsed, and
       the derived escapes end in an auto-firing element, so a markup PoC runs at insertion;
     - url_fire runs a URL's code only for the `javascript:` scheme, when the navigation happens. Where the host
       performs that navigation too (§7.4.2.2 "Beginning navigation" step 16), the marker is reached twice;
       record_sink is keyed on (class, source), so the finding is written once.
   The Trusted Types column is the spec's: TT §3.8 makes the markup sinks TrustedHTML sinks and eval a
   TrustedScript sink, and a javascript: navigation is no TT sink, hence -1. A parked candidate's sink rebinds
   by name in solve_resume_candidate, which hands back this table's own name pointer. */
struct SinkClass {
    const char       *name;      /* the display name a report and a parked entry carry */
    const char      **vectors;   /* the fixed breakouts of a single-context sink, NULL where `derive` builds them */
    int               derive;    /* SINK_DERIVE_*: which parser reads this sink's own output for its context */
    /* The CspInlineType (CSP §4.2.3's `type`) a fired breakout turns on, or -1: eval is governed by no inline
       check but by §4.4.1's string-compilation question, which has no element, no type and no §6.8.2 mapping. */
    int               policy;
    int               tt;        /* the TrustedTypeKind gating this sink, or -1 — the spec makes it no TT sink */
    /* Whether a fired breakout becomes a queued program of the flow (markup: an auto-firing handler in the real
       parse; URL: an address that is still a javascript: URL) rather than being run by the sink itself (eval,
       §19.2.1 / §20.2.1.1.1). It decides whether `fires` is a number this class can have. */
    int               queues_fire;
    const char       *fires_on;  /* what makes the fired breakout RUN, from the oracle above */
};
static const SinkClass SINKS[] = {
    [SINK_EVAL] = { "eval",      NULL,      SINK_DERIVE_JS,   -1,                          TRUSTED_TYPE_SCRIPT, 0, "sink-evaluates" },
    [SINK_HTML] = { "innerHTML", NULL,      SINK_DERIVE_HTML, CSP_INLINE_SCRIPT_ATTRIBUTE, TRUSTED_TYPE_HTML,   1, "parse-insert"   },
    /* §6.8.2 maps the inline type "navigation" to `script-src-elem`, not to `script-src-attr`, so
       `script-src 'unsafe-inline'; script-src-attr 'none'` must not block a javascript: URL. */
    [SINK_URL]  = { "location",  CANDS_URL, SINK_DERIVE_NONE, CSP_INLINE_NAVIGATION,       -1,                  1, "navigation"     },
};
#define SINK_CLASS_N ((int)(sizeof SINKS / sizeof SINKS[0]))

/* The delivery probe's token. The context probe answers which state the attacker's bytes land in, and is ASCII
   alphanumeric so it cannot change the parse it measures; this probe answers which bytes arrive at all, so it
   carries `<` and has to be a separate probe. It is this file's and not a class's, because byte provenance has
   no language: one token serves every class and is read at the class-independent point (filter_survived). */
#define SOLVE_BYTES_LOCATOR "apiclientbytes"

/* Build the delivery probe: `apiclientbytesK` followed by the K'th byte of `encodes`, for each byte, so the
   answer is read per byte instead of aligned through a mangled string. Percent-encoded, dropped,
   entity-escaped and moved all read as "not delivered" (solve_filter.h says why). `encodes` is the source's
   own declaration from concolic.c's registry. Caller frees. */
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

/* The context probe a derived class's search opens with: an inert locator injected in place of a breakout, so
   one re-run of the real page shows where the attacker's bytes land after the page's own filters,
   concatenations and re-encodings. It is a candidate flow like any other and counts in `tried`. */
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

/* CHECK, not DCHECK: every report row is written from this table, so an index it does not have would read past
   its data. */
static const SinkClass *sink_class(int sink) {
    CHECK(sink >= 0 && sink < SINK_CLASS_N,
          "an @S record named a sink class this table does not have — the whole report is written from it");
    return &SINKS[sink];
}
static const char *sink_name(int sink) { return sink_class(sink)->name; }

/* The class of a candidate flow's `cand_sink`. The flow holds this table's own name pointer, so the binding is
   identity rather than a string compare, and any other pointer is a candidate this table did not seed. */
static int sink_class_of_name(const char *name) {
    int i;
    for (i = 0; i < SINK_CLASS_N; i++) if (SINKS[i].name == name) return i;
    DFAIL("a candidate flow carried a sink name that is not one of the sink classes' own — the name is this "
          "table's pointer, so a flow holding another was built somewhere that does not go through it (a cold "
          "resume rebinding a parked candidate BY NAME must land back on this table's row)");
    return -1;
}

/* Defined after SINKS so the one entry every session passes through can assert the table. */
void solve_init(JSContext *ctx) {
    g_pending = NULL; g_pending_n = g_pending_cap = 0;
    g_cands_seeded = 0;
    g_sink_reached = g_sink_tainted = g_sink_suppressed = 0;
    g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
    /* Every row is whole and gets its breakouts from exactly one source: a class with both would derive and then
       also spray its list, and one with neither is seeded nothing and parks forever. `tt` may be -1, since the
       standard makes the URL sink no Trusted Types sink. */
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
    /* The two context locators partition their probes, so neither may contain the other: a page that writes one
       source into both an eval sink and a markup sink runs each probe past the other class's sink, and
       candidate_search declines that write by class. */
    DCHECK(!strstr(SOLVE_JS_LOCATOR, SOLVE_HTML_LOCATOR) && !strstr(SOLVE_HTML_LOCATOR, SOLVE_JS_LOCATOR),
           "the @S markup and JS context locators are not distinct — one contains the other, so the substring "
           "test that routes a probe's output to its own derivation answers for both");
    /* The delivery locator is a third, distinct from both, so a probe built to carry `<` is never routed into a
       context derivation. */
    DCHECK(!strstr(SOLVE_BYTES_LOCATOR, SOLVE_HTML_LOCATOR) && !strstr(SOLVE_HTML_LOCATOR, SOLVE_BYTES_LOCATOR) &&
           !strstr(SOLVE_BYTES_LOCATOR, SOLVE_JS_LOCATOR)   && !strstr(SOLVE_JS_LOCATOR, SOLVE_BYTES_LOCATOR),
           "the @S byte-delivery locator is not distinct from a context locator — one contains the other, so a "
           "probe built to carry the bytes a source encodes would be routed into a context derivation and the "
           "state it reported would be a state of the probe rather than of the page");
    DCHECK(!strstr(SOLVE_BYTES_LOCATOR, "X9"),
           "the @S byte-delivery locator carries the fire marker's own bytes — the probe is inert by "
           "construction and a marker in it would be recorded as a breakout arriving at its sink");
    /* The eval sink's seam. Markup and URL sinks are reached from the host components that perform them; eval and
       the Function constructor are ECMAScript intrinsics, so the engine announces them (JSEvalSinkFunc) and the
       detector is registered here, beside the marker they fire. */
    JS_SetEvalSinkHook(solve_eval_sink);
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "X9", JS_NewCFunction(ctx, js_x9, "X9", 0));
    JS_FreeValue(ctx, g);
}

/* The search for (source, class), or NULL. This is the read half, separate from sink_search because a lookup
   that created an entry would fabricate a search with no breakouts, no path and nothing to seed, which every
   later reader of g_pending would take for a detected sink. */
static Cand *search_of(const char *src, int sink) {
    for (int i = 0; i < g_pending_n; i++)
        if (g_pending[i].sink == sink && !strcmp(g_pending[i].src, src)) return &g_pending[i];
    return NULL;
}

/* Find or create the search for (source, class), saying in `*created` which it did. Creating the slot is not
   opening the search: only detection opens one (add_pending), because a cold resume re-registers a candidate
   of a search an earlier session opened. Named for what it returns, since `pending_entry` is pending.h's
   reply register. */
static Cand *sink_search(const char *src, int sink, int *created) {
    Cand *e;

    DCHECK(src && created, "a sink was registered as pending with no source, or with nowhere to say whether "
                           "this call is the one that opened its search");
    *created = 0;
    for (int i = 0; i < g_pending_n; i++)
        if (g_pending[i].sink == sink && !strcmp(g_pending[i].src, src)) return &g_pending[i];
    if (g_pending_n >= g_pending_cap) { g_pending_cap = g_pending_cap ? g_pending_cap * 2 : 8; g_pending = realloc(g_pending, (size_t)g_pending_cap * sizeof(Cand)); CHECK(g_pending, "solve: OOM pending"); }
    /* The whole slot at once. `g_pending` is realloc'd and never zeroed, so a new slot holds allocator memory. A
       compound literal assigned wholesale zero-initialises every member it does not name, so an omitted member
       is unspellable rather than audited. Zeroing at the grow instead would turn a missed member into a
       plausible 0 rather than loud garbage, and would still leave the -1 members to name.
       Every latch, ratchet, observation count and size in Cand has blank 0. The members named are those whose
       blank is not 0: `surv_at`/`surv_out` here, and `deliv`, whose blank is all ones and which the
       solve_delivered_all call below writes as part of this birth. A further such member is the one edit that
       must be made here.
       The root is not learned here. The creating callers reach it by their own routes (add_pending off the
       value, solve_resume_candidate off the park record) and state it before the entry is visible; the third
       caller, candidate_search, asserts that it created nothing. */
    e = &g_pending[g_pending_n++];
    *e = (Cand){
        .src = strdup(src),
        .sink = sink,
        /* No run has been observed yet: -1 rather than 0, as in FilterObs, because 0 is a real offset. */
        .surv_at = -1, .surv_out = -1,
    };
    CHECK(e->src, "solve: OOM pending");
    /* Everything delivers until something contradicts it, the sound-only direction (solve_filter.h): a search
       told nothing keeps every arm. The root's carrier refusals are knowable without a run, but the root is
       not known here, so cand_learn_root seeds that half. Asserted, because this is the one member the literal
       cannot state: a missing fill is an all-zero table, which refuses every byte, withdraws every candidate at
       both doors and reads as a search whose every payload the root contradicted. */
    solve_delivered_all(&e->deliv);
    DCHECK(solve_delivered_ok(&e->deliv, "<>\"'&"),
           "a freshly opened @S search refused bytes a breakout is made of — the birth above writes every "
           "member of Cand whose blank is 0 and this table's blank is all ONES, so an all-zero table here is a "
           "missing solve_delivered_all rather than a narrowing: it would withdraw every candidate at both "
           "doors and report a search whose every payload the root had contradicted");
    *created = 1;
    return e;
}

/* Add a candidate to a search, deduped by its text: a probe run reaches one sink as often as the page writes
   it, two occurrences can land in one state, and a duplicate would cost a whole re-run of the page that can
   only reproduce a result already had. */
static void push_breakout(Cand *e, const char *payload, int kind) {
    DCHECK(e && payload && *payload, "a breakout was queued onto no sink, or with no bytes in it");
    /* The kind is the pusher's statement and cannot be recovered from the bytes afterwards. The seeder, the
       arrival assert and the report each answer differently for a probe and an escape, so a third value is
       refused. */
    DCHECK(kind == CAND_PROBE || kind == CAND_ESCAPE,
           "a candidate was queued as neither an instrument nor an attack — a probe is inert and is never "
           "withdrawn and never counted as an arrival, an escape is the opposite on all three, and there is no "
           "third thing for this list to hold");
    /* An entry keeps the kind it was first pushed with. The producers that can collide (a written-down vector,
       a derivation, a re-derivation under a narrowed table) all push escapes, while a probe is built from this
       file's own locators, which no derivation emits. Asserted, because that argument rests on the locator
       vocabulary and a class added later owns its own. */
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
    /* Born whole, for sink_search's reason: `pl` is realloc'd and never zeroed. `surv` has blank 0, so the
       literal names only what this call computes. */
    e->pl[e->npl] = (CandPayload){ .bytes = strdup(payload), .kind = kind };
    CHECK(e->pl[e->npl].bytes, "solve: OOM recording a breakout for a sink search");
    e->npl++;
}

/* How many of this search's candidates are probes, read off their labels: the report's `probes`. */
static int cand_probes(const Cand *e) {
    int n = 0;
    DCHECK(e != NULL, "the probe count was asked of no search");
    for (int i = 0; i < e->npl; i++) if (e->pl[i].kind == CAND_PROBE) n++;
    return n;
}

/* Does this search hold the delivery probe? Read off its entries by the probe's own locator, the partition
   observe_delivery routes on, rather than by restating add_pending's rule for pushing one. It makes
   `deliveryProbed` absent rather than 0 for a search that has no such probe: a single-context class, or a
   source that declares no percent-encode set. */
static int cand_has_delivery_probe(const Cand *e) {
    DCHECK(e != NULL, "the delivery-probe question was asked of no search");
    for (int i = 0; i < e->npl; i++)
        if (e->pl[i].kind == CAND_PROBE && e->pl[i].bytes &&
            !strncmp(e->pl[i].bytes, SOLVE_BYTES_LOCATOR, sizeof SOLVE_BYTES_LOCATOR - 1)) return 1;
    return 0;
}

/* Has this search constructed an escape? Asked of the entries, so it holds whatever order the producers push in. */
static int cand_has_escape(const Cand *e) {
    DCHECK(e != NULL, "the escape question was asked of no search");
    for (int i = 0; i < e->npl; i++) if (e->pl[i].kind == CAND_ESCAPE) return 1;
    return 0;
}

/* The kind of these exact bytes in this search's record, or 0 for bytes it does not hold. The cold tier is the
   one legitimate producer of 0: a resumed candidate's payload rides the flow rather than this record (solve.h,
   on `payloads` being empty beside a non-zero `tried`), unless this session derived the same bytes, which is
   then the same row. Callers read 0 against Flow.cand_resumed, never as "not found". */
static int cand_kind_of(const Cand *e, const char *bytes) {
    DCHECK(e != NULL && bytes != NULL, "a candidate's kind was asked of no search, or about no bytes");
    for (int i = 0; i < e->npl; i++) if (!strcmp(e->pl[i].bytes, bytes)) return e->pl[i].kind;
    return 0;
}

/* Can this search account for the bytes that just arrived at its sink? breakout_arrived's condition, as one
   side-effect-free call so it can sit inside the DCHECK and cost nothing in a release build (check.h). */
static int cand_arrival_is_attack(const Cand *e, const Flow *f) {
    int kind;
    DCHECK(e != NULL && f != NULL && f->cand_payload != NULL,
           "the arrival question was asked of no search, or of a flow carrying no payload — the caller has "
           "already CHECKed both, so reaching here without them means a second route into this rung");
    kind = cand_kind_of(e, f->cand_payload);
    return kind == CAND_ESCAPE || (kind == 0 && f->cand_resumed);
}

/* The first of a source's declared bytes its delivery table refuses, or 0: cand_learn_root's two-sided check as
   one side-effect-free call, for cand_arrival_is_attack's reason. 0 is never a declared byte, since bytes_probe
   writes each declared byte into a NUL-terminated probe and every declared set is printable. */
static int declared_byte_refused(const SolveDelivered *d, const char *enc) {
    int i;
    for (i = 0; enc && enc[i]; i++)
        if (!d->ok[(unsigned char)enc[i]]) return (unsigned char)enc[i];
    return 0;
}

/* The search learns how its attacker bytes arrive, from the value that arrived. One envelope states one
   percent-encode set and one address component, so a second, different root is refused. A derivation over
   several operands unions their roots (concolic.h's derived_root_join) while taking `src` from the first, so
   one injection identity can bring two roots here.
   Named residual: not covered — an injection identity whose value entered through two components; next diff —
   one candidate seeded per declaring member, each carrying its own envelope, and the one that fires emitted
   (the remedy root_declared_row and concolic_deliver also name); absence shows as this assert firing on a
   value composed from two attacker sources. */
static void cand_learn_root(Cand *e, const char *root) {
    DCHECK(e && root, "a sink search was told how its bytes arrive by nothing, or was told nothing");
    if (!e->root) { e->root = strdup(root); CHECK(e->root, "solve: OOM recording a sink's delivery root"); }
    else DCHECK(!strcmp(e->root, root),
           "one sink search has been handed two different delivery ROOTS for one injection identity — the root "
           "is a SET a derivation UNIONS rather than replaces, so one injection identity CAN name a value that "
           "entered through two components — and one envelope states one percent-encode set and one address "
           "component, so the report would carry whichever detection "
           "ran last");
    /* The delivery table takes the half no run can give it here, at the one moment the root becomes known,
       which both doors pass through (add_pending, solve_resume_candidate); on a found entry this call still
       seeds. What is seeded is not the declared encode set: what the browser percent-encodes is a prior that
       only a run settles (a page may decode its own fragment), while a byte a constrained carrier refuses never
       enters the page's program and no run can widen it. concolic.c owns that column, so it answers. The
       callers push vectors and probes after this, so the table is complete before anything is queued against
       it. `deliv_seen` is untouched: a declaration-narrowed table is not a measurement. */
    /* The other column of the same row: a carrier must not refuse a byte its row also declares, since the
       declared set is the printable bytes the production excludes and the refusal is everything outside
       printable US-ASCII. A seed that cleared a declared byte would leave the delivery probe built from it
       unable to learn anything about that byte, because the table only narrows. Asked only where the seed
       fired. */
    if (concolic_source_carrier_bytes(e->root, &e->deliv))
        DCHECKF(declared_byte_refused(&e->deliv, concolic_source_encodes(e->root)) == 0,
                "an @S source's carrier refuses byte 0x%02X while the same row DECLARES it as one the "
                "component percent-encodes — the declared set is the PRINTABLE bytes the carrier's own "
                "production excludes and the refusal covers everything outside printable US-ASCII, so the two "
                "cannot name one byte. The delivery probe is built out of the declared set, so this seed has "
                "just withdrawn the instrument that was going to measure it",
                (unsigned)declared_byte_refused(&e->deliv, concolic_source_encodes(e->root)));
}

/* Has this search fired? Derived from the finding store, where a fire is recorded (record_sink), with
   record_sink's own dedup comparison, so the two cannot disagree. Asked of the fire rather than of `reinject`
   or cand_has_escape, both of which a cold-resumed search falsifies: it is born without a path, and its
   payload rides the flow with no row in `pl`. */
static int search_solved(const Cand *e) {
    DCHECK(e != NULL && e->src != NULL,
           "the solved question was asked of no search, or of one with no injection identity — the finding "
           "store is keyed by (class, source) and a search with no source could not be found in it, so the "
           "answer would be a confident `no` about a search that may well have fired");
    for (int i = 0; i < g_sinks_n; i++)
        if (g_sinks[i].cls == e->sink && !strcmp(g_sinks[i].source, e->src)) return 1;
    return 0;
}

/* Declared here because add_pending, above the definition, asks whether a search still seeds. */
static int search_seeds(const Cand *e);

/* Copy and free one PrincipalGate's rows. What a row owns is concolic.c's (concolic_pred_copy /
   concolic_pred_release); these own the array, the same division endpoint.c's param_pred_copy /
   param_pred_free make over the same row type. */
static ConcolicPred *gate_pred_copy(const ConcolicPred *src, int n) {
    ConcolicPred *out;
    int i;

    DCHECK(n > 0 && src != NULL,
           "a principal's predicate rows were copied with no rows to copy — the walk that hands them over "
           "calls back only where it found some, so an empty set here is a caller that invented one");
    out = malloc((size_t)n * sizeof *out);
    CHECK(out, "solve: OOM taking the demands a path made of an attacker's principal off the flow");
    for (i = 0; i < n; i++) concolic_pred_copy(&out[i], &src[i]);
    return out;
}

static void gate_pred_free(ConcolicPred *p, int n) {
    int i;
    for (i = 0; i < n; i++) concolic_pred_release(&p[i]);
    free(p);
}

/* One principal's demands, called back by concolic_principal_preds once per principal the path tested. It
   appends, because the demands of one path conjoin. */
static void cand_gate_row(void *user, const char *src, const ConcolicPred *pred, int n) {
    Cand *e = (Cand *)user;
    PrincipalGate *a;

    DCHECK(e != NULL && src != NULL && *src && pred != NULL && n > 0,
           "a principal's demand arrived for no search, with no principal named, or with no rows — the three "
           "are one observation written together by the walk, so a call holding some of them describes a "
           "demand nothing can answer for");
    a = realloc(e->pg, (size_t)(e->npg + 1) * sizeof *a);
    CHECK(a, "solve: OOM recording the demands a path made of an attacker's principal — a lost demand reports "
             "an identity an attacker can hold for a path that demands one they cannot");
    e->pg = a;
    e->pg[e->npg].src = strdup(src);
    CHECK(e->pg[e->npg].src, "solve: OOM naming the principal a path made a demand of");
    e->pg[e->npg].pred = gate_pred_copy(pred, n);
    e->pg[e->npg].npred = n;
    e->npg++;
}

/* Decide what the path every candidate of this search replays demanded of an attacker's principal: the verdict
   and its evidence, written together while the flow that froze the path exists. */
static void cand_learn_principal_gates(Cand *e) {
    DCHECK(e != NULL, "the demands of a path were decided for no search");
    DCHECK(e->pg_demand == PG_UNEXAMINED && e->npg == 0,
           "a search's principal demands were decided twice — this runs past the ONE capture's own two "
           "returns, so a second verdict describes a path this search no longer stands on, exactly as a second "
           "recorded length would");
    /* A pinned principal is stated as PG_UNFORGEABLE rather than left as an empty set. detect_sink's suppression
       stops a detection from opening a search, but this capture also has the witness door (derive_from_witness,
       a resumed search's own context probe), which that suppression never sees. A path's demands conjoin, so
       reporting only its forgeable ones, or an empty set a consumer cannot tell from "demanded nothing", would
       state a satisfiable identity for an unsatisfiable path. The sink stays reportable through any sibling
       flow that reaches it without the demand. */
    if (concolic_principal_pinned()) { e->pg_demand = PG_UNFORGEABLE; return; }
    concolic_principal_preds(cand_gate_row, e);
    /* The walk calls back exactly where it found a demand, so `npg` is now the verdict; deriving it here keeps
       the enum and the rows from being two producers that can disagree. */
    e->pg_demand = e->npg ? PG_FORGEABLE : PG_NONE;
}

/* Take the re-injection path of the flow standing at this search's sink: the one capture, reached from two
   doors. One is detection (add_pending). The other is this search's own context probe coming back
   (derive_from_witness), the only door a cold-resumed session reaches, since a verifying flow does not
   detect; without it a resumed search never held a path and queue_derived dropped every escape derived from
   its witness. Either flow demonstrably reached this sink with this source, which is the only property a
   replayed path needs. It cannot re-open a fired search, because the guard is the fire itself
   (search_solved), and it pushes nothing, which keeps solve_resume_candidate's door free of seeding. */
static void cand_learn_path(Cand *e) {
    DCHECK(e != NULL, "a re-injection point was taken for no search");
    if (e->reinject) return;       /* the one capture has happened, at whichever door reached it first */
    if (search_solved(e)) return;  /* a solved search seeds no further candidates — see record_sink */
    e->reinject = decide_freeze_path();
    /* And the path's length, taken here where the path exists: it is what `runwayArms` reports, and record_sink
       gives the blob back at the fire, so a length read later would be 0 for the searches that succeeded.
       decide_freeze_path CHECKs its allocation and never returns NULL, so the accessor's own CHECK adds no
       release failure mode. A length of 0 is a legitimate answer, from a flow that decided nothing: the
       `runwayArms:0` reading. */
    {
        long arms = 0;
        DCHECK(e->reinject_len == 0,
               "a search's recorded path length was written twice — the pointer tested above is what makes "
               "this the ONE capture, so a second write means this entry reached the capture holding no path "
               "while a length from an earlier one still stood, and that first value described a path this "
               "search no longer stands on");
        decide_blob_stats(e->reinject, &arms, NULL);
        DCHECK(arms >= 0 && arms <= 0x7fffffff,
               "a frozen decision path reports a length that is not a count of slots — `runwayArms` is read "
               "as whether this search offered its candidates any arm at all, so a negative value or one "
               "truncated by the store would publish that answer on something that is not a length, and 0 is "
               "the reading the whole pair turns on");
        e->reinject_len = (int)arms;
    }
    /* And what that path demanded of an attacker's principal, taken now because the registry name and the
       constraint rows are borrowed from this flow. It runs past the capture's two returns, so a search that
       already holds a path already holds that path's demands. */
    cand_learn_principal_gates(e);
}

/* A detected sink opens its search: a single-context class states its breakouts, any other class the probes
   whose runs its derivation reads. */
static void add_pending(const char *src, const char *root, int sink) {
    int created = 0;
    Cand *e = sink_search(src, sink, &created);
    const SinkClass *sc;

    /* Before the early returns, because most detections find this entry rather than create it; on a found entry
       this is the equality assert that says both ends of the cold tier agree on how these bytes arrive. */
    cand_learn_root(e, root);
    /* Opening is a property of the search, not of who made the slot. In a resuming session cold_resume runs at
       engine init and creates the slot before any detection, so a `created` test here would leave the search
       with no probe, no vectors and no path, and the report would say `payloads:[]`. */
    if (e->opened) return;
    /* A search that already fired is not opened either. The cold tier can finish a search before this session's
       first detection, when a resumed candidate fires before any exploration flow re-reaches the sink; opening
       it would pay a credit for nothing new, push a probe nobody seeds and then assert a path record_sink has
       taken back. This is not a seen-set: what closes a search is emitted output, the detecting flow runs on,
       and detect_sink has already counted the arrival. */
    if (search_solved(e)) return;
    /* A detection opening an entry it did not create means the cold tier made it, which leaves one of two
       numbers: a resumed record raises `tried`, a withdrawn one raises `resumed_withdrawn`. Any other entry was
       made by a third door. */
    DCHECK(created || e->tried > 0 || e->resumed_withdrawn > 0,
           "a sink search is being opened on an entry this detection did not create and no cold-resumed "
           "candidate accounts for — g_pending has two writers, detection and the cold tier's rebuild, and the "
           "second either RESUMES a record (raising `tried`) or WITHDRAWS one whose carrier refuses its "
           "payload (raising `resumed_withdrawn`), so an entry that predates this call with neither was put "
           "there by a third door and is about to be handed a context probe and a re-injection point on behalf "
           "of a search nobody opened");
    e->opened = 1;
    /* The re-injection path is taken here for every class, at the moment a flow stands at this sink holding the
       value that reached it: a candidate with no path re-forks the document's whole gate tree (2^K flows behind
       K gates) whatever seeded it. It is the detecting flow's path, sound for every candidate for the reason
       given at Cand.reinject. cand_learn_path is the one capture; derive_from_witness is its other door. */
    DCHECK(flow_running() != NULL,
           "an attacker source reached a sink with no flow running — a concolic value is minted by a flow and "
           "carried by one, so there is no route to this line from outside the scheduler, and the path about to "
           "be frozen would be whatever chain the previously-switched-in flow left behind");
    /* The flow that made the observation is paid for it. The credit is at the open rather than at the slot's
       creation, because a cold resume creating the slot observes nothing and runs with no flow; it sits below
       the assert so a missing flow aborts instead of silently skipping the payment. */
    flow_credit_emit(1.0);
    cand_learn_path(e);
    sc = sink_class(sink);
    /* A single-context class's written-down vectors are attacks: the seeder may withdraw a contradicted one, and
       the report marks it. */
    if (sc->vectors) { for (int c = 0; sc->vectors[c]; c++) push_breakout(e, sc->vectors[c], CAND_ESCAPE); }
    else {
        /* The probes, labelled as probes, so the label rather than the push order carries the fact. */
        push_breakout(e, derive_probe(sc->derive), CAND_PROBE);
        /* And the delivery probe, only for a deriving class: its table is read by a derivation choosing between
           spellings of one exit, which a class with written-down vectors does not have. Skipped where the source
           declares no percent-encode set, such as server-injected page state (`window.__FLAGS`), since no byte's
           arrival is then in question. */
        {
            const char *enc = concolic_source_encodes(root);
            if (enc && *enc) {
                char *bp = bytes_probe(enc);
                push_breakout(e, bp, CAND_PROBE);
                free(bp);
            }
        }
    }
    /* Asked through search_seeds, this file's one spelling of the question. */
    DCHECK(e->npl > 0 && search_seeds(e),
           "a search was opened with no candidate to run or with no path to run it on — the class states one of "
           "the two breakout sources (solve_init asserts the exclusive or) and this call is the one moment a "
           "flow stands at the sink, so either missing means the search would re-search the document's whole "
           "gate tree for an arm the detection already took. A search record_sink has CLOSED is not a "
           "counterexample and never reaches this line: it is refused at the solved return above, which is "
           "what lets the release that clears this path go on being the closure");
}

static void record_sink(int cls, const char *source, const char *poc) {
    /* A finding is a pending search that solved, so the two lists are one list in two states: the parked emit
       subtracts findings by (class, source), and a finding with no pending twin would report as both fired and
       parked. The twin also supplies the delivery root, because at the marker the value that carried the bytes
       is long since concrete. Both checks are CHECK because both pointers are dereferenced below in release. */
    Cand *twin = search_of(source, cls);
    CHECK(twin != NULL, "an @S finding was recorded for a sink that was never detected as pending");
    /* Both doors into g_pending state the root (detection off the value, a cold resume off the park record), so
       a NULL here names a third door. */
    CHECK(twin->root != NULL,
           "the search behind a fire-verified @S PoC never learned how its bytes arrive — the finding is the "
           "strongest thing this half of the tool emits, and without the root §S(d)'s reproduction envelope "
           "reports it as an exploit no navigation reaches");
    sink_class(cls);   /* the row exists before anything is stored against it */
    for (int i = 0; i < g_sinks_n; i++) if (g_sinks[i].cls == cls && !strcmp(g_sinks[i].source, source)) return;
    if (g_sinks_n >= g_sinks_cap) { g_sinks_cap = g_sinks_cap ? g_sinks_cap * 2 : 8; g_sinks = realloc(g_sinks, (size_t)g_sinks_cap * sizeof(Finding)); CHECK(g_sinks, "solve: OOM @S store"); }
    Finding *f = &g_sinks[g_sinks_n++];
    /* Born whole, for sink_search's reason: `g_sinks` is realloc'd and never zeroed. Every blank here is 0 or
       NULL, so the literal names only what is computed. */
    *f = (Finding){
        .cls = cls,
        .source = strdup(source ? source : "?"),
        .poc = strdup(poc),
        .root = strdup(twin->root),
    };
    /* A half-stored finding is a corrupt one, not a lost one: `solved` strcmps the source, and solve_json_array
       writes the PoC and the root straight into the report. */
    CHECK(f->source && f->poc && f->root,
          "solve: OOM storing a fire-verified @S PoC — the finding is the proof, and a half-stored one corrupts "
          "every later read of the report rather than losing it. The delivery ROOT is part of that proof: §S(d) "
          "requires every emitted PoC to carry its reproduction envelope, and a finding that lost its root "
          "reports as one no navigation reaches");
    /* The flow that proved the exploit is paid for it, as add_pending pays the detection. A candidate flow
       records no endpoints (endpoint_suppress), so without this its reward would rest on the optimism term. */
    flow_credit_emit(1.0);
    /* Give back the re-injection point: the blob pins the frozen segment and every segment below it, and a
       solved search seeds nothing further. Clearing it is what closes the search, since search_seeds reads this
       NULL; the probe's other arms keep arriving after a fire and would otherwise be seeded again. */
    if (twin->reinject) { decide_blob_free(twin->reinject); twin->reinject = NULL; }
}

/* Detection: the tail the three sink classes share, and the last point at which the value that carried the
   attacker's bytes exists. The search is keyed by the value's injection identity, derived ones included,
   because that is where a substitution has to land; how the bytes arrived is asked with the value's root,
   which every concolic carries (concolic_alloc asserts provenance and root together). */
static void detect_sink(JSValueConst arg, int cls) {
    const char *shape, *src, *root;

    /* The arrival census is counted here, where the three classes converge, and the concolic test with it, so a
       new class cannot forget either. An empty @S surface means one of four things that take opposite actions:
       no attacker source was read (counted where concolic.h mints one), no sink ran, sinks ran on the page's
       own strings, or tainted input arrived and its search was suppressed as unforgeable, which is a positive
       result. The counts are exploration-only because every caller returns inside its own is_verifying()
       branch first, so a candidate's own bytes coming back are never counted as the page delivering taint.
       The one rule that is not every class's is asserted here rather than asked: ECMAScript §19.2.1.1
       PerformEval ( source, strictCaller, direct ) step 2 makes a concrete non-string offered to eval a call
       that compiles nothing. solve_eval_sink declines it; markup and URL sinks run ToString, so an object with
       a toString is a real vector there and must not be declined. */
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
    /* Can an attacker be the one standing here (the unforgeable-origin rule), asked while the flow that arrived
       still exists. What is suppressed is the search, not the arm: the flow runs on, since the page really does
       reach this sink when a message from the pinned origin arrives, but a PoC no cross-document attacker can
       deliver is a false one. No entry is emitted, since a parked entry would say "not solved yet"; the sink
       stays reportable through any other flow that reaches it without the demand. The return is counted,
       because it is a decision rather than an absence. */
    if (concolic_principal_pinned()) { g_sink_suppressed++; return; }
    add_pending(src ? src : (shape ? shape : "?"), root, cls);
}

/* Does this search still seed? The re-injection path is the answer: a search holds one from the moment a flow
   standing at its sink gives it one (cand_learn_path, at detection or when its own context probe returns)
   until record_sink closes it at the fire. A cold-resumed search acquires a path the same way, so it seeds,
   derives and reports like one detection opened, except that a search the cold tier already solved is never
   opened (add_pending). Not a seen-set: what closes a search is a fire-verified PoC for this exact (source,
   sink), which record_sink would not store twice, so what is declined is a re-run whose only possible result
   is that discard. */
static int search_seeds(const Cand *e) {
    DCHECK(e != NULL, "the seeding question was asked of no search");
    return e->reinject != NULL;
}

/* The derivation's callback for each escape it constructs from a context witness: the escape joins this sink's
   search and the next drain seeds it. The witness is the string the page's own code built around the probe's
   inert locator, with its concatenations, filters and re-encodings all run, so each escape is that state's
   real exit transition. */
static void queue_derived(void *user, const char *breakout) {
    Cand *e = (Cand *)user;

    /* A closed search takes no more breakouts: the probe's other arms keep arriving here after a fire. */
    if (!search_seeds(e)) return;

    /* The capture is not here: cand_learn_path takes the path one call above the derivation, so a search that
       receives a breakout already holds one. This is the two-sided half of the delivery constraint: the
       derivation is handed this search's table and constructs within it, so an escape carrying a refused byte
       is a derivation that ignored the constraint. Deliverability is asked again where it can have changed
       under a queued spelling, because the table is measured and narrows later: at seeding
       (solve_seed_candidates withdraws) and at arrival (breakout_arrived withholds the credit). */
    DCHECK(solve_delivered_ok(&e->deliv, breakout),
           "a derived @S breakout carries a byte this search has OBSERVED does not reach its sink — the "
           "derivation is given the same table and emits nothing outside it, so this escape was constructed "
           "past the constraint and would spend a document re-run to arrive transformed");
    push_breakout(e, breakout, CAND_ESCAPE);
}

/* Keep the string a context probe's run handed this sink, so the derivation can be re-run on it. Deduped by
   text: the same template rendered twice gives the same witness. */
static void learn_witness(Cand *e, const char *out) {
    DCHECK(e != NULL && out != NULL && *out,
           "a sink search was handed a context witness with no bytes in it — the witness is what a state is "
           "read off, and an empty one would derive a context for a write that never happened");
    /* A witness is produced by one of this search's candidate flows reaching the sink, and solve_flow_begin
       counts a turn at every switch-in of one, so `turns` is already nonzero; otherwise `reached:0,turns:0`
       would read as never scheduled for a search whose probe has run the whole document. */
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

/* Derive this search's breakouts from its witnesses: ECMAScript §12 for the eval sink, §13.2.5 for the markup
   one, each read by the parser that owns the sink's language, under the search's delivery table. There are
   two callers: the context probe arriving, and a delivery observation narrowing the table, which re-derives
   from the stored witnesses. That is the near-miss mutation performed by re-derivation, so every escape is
   still the state's own exit and nothing is invented; what comes back joins the search (push_breakout
   dedups) and the next drain seeds it, and a table that yields no new spelling pushes nothing. The
   derivations' return counts are read: an escape constructed but not held by the search afterwards was
   dropped, and the report would state `probes == payloads`. */
static void derive_from_witness(Cand *e) {
    int derive, built = 0;

    DCHECK(e != NULL && e->nwit > 0,
           "a derivation was asked to run on a search that holds no witness — the witness is the string the "
           "context probe's own run handed this sink, so without one there is no observation to read a state "
           "off and the re-derivation would be a static shape of the expression");
    /* The second door onto the one capture (cand_learn_path): a witness means one of this search's candidate
       flows reached this sink with this source. It is a no-op for a search this session detected; for a resumed
       one it is the only capture, without which queue_derived would drop every escape built below. Taken before
       the derivation, because the hand-off asserted below needs it. */
    cand_learn_path(e);
    derive = sink_class(e->sink)->derive;
    /* The routed classes are named rather than reached by an `else`, so in a release build a class with no
       routed parser constructs nothing instead of handing HTML-tokenizer bytes to the JS derivation. The DFAIL
       is dev-only because SINKS is a static of this build whose columns solve_init asserts at startup. */
    if (derive != SINK_DERIVE_HTML && derive != SINK_DERIVE_JS)
        DFAIL("a sink class stored a context witness and declares no derivation to read it with — a class "
              "whose breakouts are written down never stores one, so this is a class whose derivation column "
              "was set without a parser being routed for it");
    for (int i = 0; i < e->nwit; i++) {
        if      (derive == SINK_DERIVE_HTML) built += solve_html_breakouts(e->wit[i], &e->deliv, queue_derived, e);
        else if (derive == SINK_DERIVE_JS)   built += solve_js_breakouts(e->wit[i], &e->deliv, queue_derived, e);
    }
    DCHECK(built == 0 || cand_has_escape(e) || search_solved(e),
           "a derivation constructed an escape that this search does not hold — the constructed count and the "
           "search's own payload list are the two ends of one hand-off, so a search left holding nothing but "
           "its probes after a derivation that built something has DROPPED it, and the report would state "
           "`probes == payloads` — which a reader takes as the positive statement that this source can carry no "
           "exit from the state its bytes landed in. The third term is the ONE drop this is not: a search "
           "closed by a fire seeds nothing further by design, and a RESUMED one that fired holds no row in "
           "`pl` to show for it, so the list is a proxy for closure that answers `no` about a solved search "
           "and the fire is the fact");
}

/* Which of the bytes the source's component percent-encodes reached the sink: the character-provenance
   observation, taken off the delivery probe's run. The probe wrote `apiclientbytesK` in front of the K'th
   declared byte, so the character after that token in the string a real re-execution handed a real sink is
   that byte or it is not. Percent-encoded, dropped and entity-escaped all read as "did not arrive", since a
   re-encoded byte cannot break a sink out of its context (solve_filter.h). A token that did not arrive says
   nothing about its byte, so uncertainty keeps the arm. A narrowing re-runs the derivation on the witnesses.
   Withdrawing entries the narrowing now contradicts is not done here: which entries those are is a pure
   function of `pl` and this monotone table, so the two sites that would spend on one
   (solve_seed_candidates, breakout_arrived) recompute it. */
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
    /* Counted before any token is looked for, which separates this observation's absence from its zero:
       reaching here is the probe's bytes turning up in a sink's string (the caller partitioned on its locator),
       and a run that finds no token is the loudest result this instrument has. */
    e->deliv_runs++;
    n = strlen(enc);
    memcpy(tok, SOLVE_BYTES_LOCATOR, tl);
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)enc[i];
        const char *p;

        tok[tl] = (char)('0' + (int)i); tok[tl + 1] = 0;
        if (!(p = strstr(out, tok))) continue;          /* this token never arrived: says nothing about the byte */
        /* The probe's own token got here, so the character after it is an observation of that byte. */
        e->deliv_seen = 1;
        if ((unsigned char)p[tl + 1] == b) continue;    /* delivered as itself */
        if (!e->deliv.ok[b]) continue;                  /* already observed, and the table only narrows */
        e->deliv.ok[b] = 0;
        changed = 1;
    }
    if (changed && e->nwit > 0) derive_from_witness(e);
}

/* A breakout of this search just arrived at its own sink. This is the one place `reached` moves, so the classes
   cannot come to disagree about what it counts. */
static void breakout_arrived(Cand *e) {
    /* The bytes that arrived are the running flow's own substitution, from the same flow candidate_search
       resolved `e` from, so they are asked of that flow. CHECK because it is dereferenced below. */
    Flow *f = flow_running();

    DCHECK(e != NULL, "a breakout arrived at no search — the caller resolved one before reading the bytes");
    CHECK(f != NULL && f->cand_payload != NULL,
          "solve: a breakout arrived at a sink with no candidate substitution on the running flow — nothing but "
          "a candidate run injects a marker, and the rung below is paid on whether THIS search still holds "
          "these exact bytes as viable, so an arrival with no payload cannot be told from one it withdrew");
    /* Could this search have produced these bytes? Asked of the entry's label, not of the list's shape:
         CAND_ESCAPE  this search built or stated these bytes as an attack, in either order its producers push;
         0            this session's record has no row for them, which only a cold-resumed candidate may cause,
                      and Flow.cand_resumed states it;
         CAND_PROBE   one of the search's own probes, which carries no marker, so the partition that routed
                      this arrival has broken and a measurement is about to be paid an arrival rung (and, at
                      the URL class, a fire). */
    DCHECK(cand_arrival_is_attack(e, f),
           "a sink recorded a BREAKOUT arriving whose bytes are not an attack of this search — they are "
           "either one of its own inert PROBES, which carries no marker and cannot fire, so the partition "
           "that routed this arrival has broken and a measurement is about to be paid an arrival rung; or "
           "they are bytes this session's record does not hold and the flow carrying them was not rebuilt "
           "from a park record, which means a candidate was assembled outside both of the search's doors "
           "(solve_seed_candidates and solve_resume_candidate) and nothing knows what it is running");
    /* The marker is a strong partition, not a proof: `X9` is two characters and a minified bundle names things
       that way, so the substitution must have entered the program. */
    DCHECK(concolic_candidate_delivered(),
           "an @S breakout was recorded as ARRIVING for a flow whose payload has not entered the program — "
           "the marker these bytes were found by is the page's own, so the ladder is about to advance a "
           "candidate for a string it never produced");
    /* And this file's own record of that event: a second door into the substitution shows up here as an arrival
       standing over `substituted:0`. */
    DCHECK(e->substituted > 0,
           "an @S breakout arrived at its own sink for a search that has never recorded a substitution — the "
           "bytes got here, so they entered the program, and concolic_deliver reports every entry to "
           "solve_observe_substitution; the parked card would state that these runs never reached their own "
           "source read");
    /* The arrival rung, written to both quantities under one question: has this search's own measurement left
       these bytes able to arrive? Inside it the comparator is written unconditionally (flow_observe_rung, so
       every candidate that reaches the sink outranks those of its search that have not) and the ledger is
       latched, paid at the search's 0->1 crossing only, which is what "new" value means; later arrivals still
       derive, fire and count. A spelling the table has since contradicted is refused both, because the rung is
       a distance to firing; this prunes a contradicted arm and is no bound, since the table narrows only on a
       positive observation. A crossing already paid to a then-viable candidate is not taken back: it paid for
       an observation that was true when made. */
    if (solve_delivered_ok(&e->deliv, f->cand_payload)) {
        flow_observe_rung(f, FLOW_RUNG_ARRIVED);
        if (e->reach_credited == 0) { e->reach_credited = 1; flow_credit_emit(1.0); }
    }
    e->reached++;
}

/* The filter-survived rung, the one sink rung that is class-independent: asked of every string any
   code-execution sink receives during a candidate run, before the class partition, because the question is
   how much of the payload is still alive wherever it surfaces. It is credited to the running flow's search,
   never the sink's. Below it, FLOW_RUNG_DELIVERED is observed at the source read (concolic.c), so rung 0 is
   bytes that never entered the program, a delivered flow at `cand_surv == 0` is bytes that survived to no
   sink, and a nonzero fraction is this measurement. The search-level pair is a ratchet: an observation that
   does not beat the best pays nothing and one that does pays the fraction it added, so the rung is worth at
   most 1.0 per search. CHECKs, because the pointers are dereferenced below. */
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
    /* The measurement's own precondition, asserted where it is made and not only where it is routed: without it
       a new route into a sink could measure the page's own strings, and a nonzero fraction of a real payload in
       a page string is indistinguishable from an observation. This asks the component (are the bytes in this
       replay's program now); flow_observe_survival asserts the flow's monotone ladder rung. A restarted
       candidate makes them differ, so neither duplicates the other. */
    DCHECK(concolic_candidate_delivered(),
           "an @S survival fraction is about to be measured for a flow whose payload has not entered the "
           "program — the run this is about to find is the PAGE'S own bytes coinciding with the candidate's, "
           "so the flow's fitness, the search's ratchet and the report's surviving-byte count would all be "
           "readings of text the attacker never supplied. The sink entry that reached here did not ask "
           "concolic_candidate_delivered");

    /* Counted before the observation is made, which separates its absence from its zero: everything below is a
       best-so-far and a zero run writes nothing, so without this a search that watched four hundred sink writes
       carrying none of its bytes reads like one that never saw a sink. A report counter, not a credit or a
       rung. Every substitution is performed in concolic_deliver and reported to solve_observe_substitution, so
       a sink observation over `substituted:0` is an unreported door into the substitution. */
    e->sink_strings++;
    DCHECK(e->substituted > 0,
           "an @S candidate's bytes reached a code-execution sink for a search that has never recorded a "
           "substitution — every delivery runs through concolic_deliver and is reported to "
           "solve_observe_substitution, so this is a second door into the substitution that does not report "
           "one, and the parked card is about to state that these runs never reached their own source read");

    /* The delivery probe is read here, at the same class-independent point, because it observes the running
       flow's own bytes wherever they surface. Its token is the partition; nothing else carries it. */
    if (!strncmp(f->cand_payload, SOLVE_BYTES_LOCATOR, sizeof SOLVE_BYTES_LOCATOR - 1))
        observe_delivery(e, out);

    solve_filter_survival(out, f->cand_payload, &o);
    DCHECK(o.len > 0, "a candidate flow carries an empty payload — see solve_filter.c's own assert");
    /* The fitness, written on this flow before the search-level ratchet, as a reading rather than a payment. The
       ratchet is the ledger and pays for one distance once, so it cannot order two candidates of one search;
       the comparator can. Taken unconditionally, above the zero return and the improvement test: a run that
       ties the search's best is no news to the ledger and all the news to the comparator. */
    flow_observe_survival(f, (double)o.run / (double)o.len);
    if (o.run == 0) return;                  /* none of this candidate is in this string: an observation of 0 */
    /* Which candidate this is, recorded before the ratchet erases the distinction. No row (idx < 0) is a
       cold-resumed candidate whose payload rides the flow rather than `pl`; the search-level pair still records
       it. Bytes this session also holds are one row for both, keyed by payload, as push_breakout dedups. */
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
    /* The offsets of the run just recorded, in the same branch, because length and position are one observation
       (solve_filter_survival reports them together). solve_filter.c checks them against this observation's
       strings; this asserts that the four numbers the search now holds agree, which the report and the mutation
       that reads which segment died depend on. */
    e->surv_at = o.at; e->surv_out = o.out_at;
    DCHECK(e->surv_at >= 0 && e->surv_out >= 0 && e->surv_at + e->surv_run <= e->surv_len,
           "the @S survival ratchet recorded a surviving segment that does not fit inside the candidate it "
           "measured, or recorded a run with no position — the offset is into the candidate and the run is a "
           "substring of it by construction, so a segment running past its own end names bytes of some other "
           "string, and the mutation these offsets exist to aim would be aimed at them");
    flow_credit_emit(now - had);
}

/* The context-escaped rung. Between arriving (`reached`) and firing (the marker calls, which only re-execution
   decides) is whether the bytes are out of the state they were written into. Each class answers from its own
   language without re-deriving: the eval sink asks the §12 scan that built the escape whether the marker
   begins an input element; the markup sink reads the marker out of an auto-firing handler in the real parse
   (HTML §8.1.1 Introduction lists "event handler content attributes" among the mechanisms that "cause
   author-provided executable code to run"); the URL sink asks whether the delivered address is still a
   javascript: URL. The ledger is paid at the search's 0->1 crossing and the comparator once per flow, as in
   breakout_arrived. The delivery table is asked here too; since it only narrows, a spelling deliverable here
   was deliverable at its arrival, which keeps flow_observe_rung's ordering assert true. */
static void escape_reached(Cand *e) {
    /* The running flow, for breakout_arrived's reason; CHECK because it is dereferenced below. */
    Flow *f = flow_running();
    /* Asked once and held, so the gate and the assert after it see the same observation. */
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
    /* breakout_arrived's split, one rung up: the comparator is written inside the gate, and the ledger is
       latched on its own field, never on the report counter `escaped`, which also counts contradicted
       spellings. */
    deliverable = solve_delivered_ok(&e->deliv, f->cand_payload);
    if (deliverable) {
        flow_observe_rung(f, FLOW_RUNG_ESCAPED);
        if (e->escape_credited == 0) {
            /* The ledger climbs its rungs in order. This crossing is paid only while the table still holds these
               bytes, the table only narrows, and flow_observe_rung refuses FLOW_RUNG_ESCAPED to a flow that has
               not stood on FLOW_RUNG_ARRIVED, so the arrival crossing is already paid. A rung added above this
               one asserts this one in the same way. */
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
    /* The two-sided half: a deliverable escape leaves with the crossing paid. Placed after the increment so it
       catches a latch read off `escaped`. */
    DCHECK(!deliverable || e->escape_credited == 1,
           "a deliverable @S breakout stood in an executable position and the search's escape crossing is "
           "still unpaid — the ledger's latch is being read off a REPORT counter, which counts the arrivals "
           "the ledger refuses to pay for, so the first contradicted escape spent the crossing that the "
           "spelling which can actually fire needed");
}

/* The search the running candidate belongs to, asked by both halves of a verifying run (the probe that derives
   and the breakout that fires), so the assertions about the candidate machinery are stated once. NULL means
   this sink is another class's, a partition rather than a swallowed condition: one source can feed two
   classes (a page writes `location.hash` into an eval sink and then into a markup sink), so an eval
   candidate's bytes reach the markup write too. Deriving there would file breakouts against a search that
   never asked, and firing there would record a markup breakout under the eval class; the other class has its
   own search over that write. */
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
    if (is_verifying()) {                          /* candidate run: the arg is the injected, concrete code */
        Cand *e;
        const char *code;
        if (concolic_is(arg)) return;           /* injection didn't reach this read -> not our candidate */
        /* And the half that test cannot say. "Not concolic" is also true of every literal the page writes, and a
           breakout is punctuation, so a run of a byte or two is found in almost any string. What answers it is
           whether this flow's substitution has been performed (concolic.h), which only the component performing
           it knows; 0 means nothing here is this flow's. This is the whole candidate arm's precondition, so the
           two sibling sinks carry the same line. It is not a bound: the same flow reaching the sink after its
           source read is measured in full. The delivery rung is written where the substitution is performed
           (concolic.c), not here, because this line stands at a sink. */
        if (!concolic_candidate_delivered()) return;
        /* A non-string is no sink: §19.2.1.1 PerformEval step 2, "If source is not a String, return source",
           compiles nothing, so there is no context to escape and no bytes to measure. The sibling sinks must not
           copy this, since a URL or markup sink runs ToString and `location = { toString(){ return
           "javascript:…" } }` is a real vector. */
        if (!JS_IsString(arg)) return;
        /* Converted before the class partition: the survival rung is about the running flow's own bytes wherever
           they surface, so this is the only place that observation exists. */
        if (!(code = JS_ToCString(ctx, arg))) return;
        filter_survived(code);
        if (!(e = candidate_search(SINK_EVAL))) { JS_FreeCString(ctx, code); return; }   /* another class's search owns this write */
        {
            /* The same partition the markup sink makes: a context probe carries the inert locator and no X9, a
               derived breakout carries X9 and no locator. It also keeps the page's own evals out of the search:
               the engine announces every eval, and one on a literal is neither this search's probe nor its
               breakout, so nothing is derived or counted, and the engine still evaluates it once, as the page
               does. Only the breakout branch is an arrival (see Cand.reached).
               The breakout branch fires nothing, because the sink itself is about to: the engine announced the
               real 19.2.1 / 20.2.1.1.1 and compiles the argument on return. A queued copy would be a second and
               weaker executor, since a direct eval runs in the caller's scope and strictness and
               CreateDynamicFunction creates a function without calling it. */
            if (strstr(code, SOLVE_JS_LOCATOR))  { learn_witness(e, code); derive_from_witness(e); }
            else if (strstr(code, "X9")) {
                const char *m;
                breakout_arrived(e);
                /* The context-escaped rung for the JS class, asked of the same §12 scan that built the escape at
                   every occurrence of the marker; the first that begins an input element answers, since the
                   rung is a boolean about the search. */
                for (m = code; (m = strstr(m, "X9")) != NULL; m += 2)
                    if (solve_js_at_source(code, (size_t)(m - code))) { escape_reached(e); break; }
            }
            JS_FreeCString(ctx, code);
        }
        return;                                 /* the marker records the PoC when the engine runs these bytes */
    }
    /* §19.2.1.1 PerformEval step 2 on the detection arm too: the engine announces every source before deciding
       anything, while `reached` counts executed code-execution sinks, and `eval(fn)`, `eval({})` and `eval(42)`
       compile nothing. Counting them would present "sinks ran on the page's own strings" for a document that
       reached no sink. A concolic is not declined: unknown external input takes step 2 and is detected here
       (solve.h), and js_eval_program_source compiles its example when it has one. The sibling sinks must not
       copy this line; detect_sink asserts the rule where all three converge. */
    if (!JS_IsString(arg) && !concolic_is(arg)) return;
    detect_sink(arg, SINK_EVAL);   /* record the source; the breakout is searched at verify */
}

/* Whether the program the host caller holds right now came out of solve_eval_sink_source: a claim about
   adjacency, like the engine's per-compile latch, so nothing that turns a string into a program may stand
   between the two. Read and cleared by solve_eval_sink_announced. */
static int g_host_sink_announced;

int solve_eval_sink_announced(void) {
    int a = g_host_sink_announced;

    g_host_sink_announced = 0;
    return a;
}

/* The host's own string-to-code step (HTML §8.7 Timers, substeps 9.8.2-9.8.8; solve.h says why it is not an
   ECMAScript eval). The announcement comes first and unconditionally: detection needs the value that is not a
   program (unknown input names no bytes) and a candidate run needs the one that is (the substituted breakout),
   so announcing only one arm would lose half the ladder. Answers the program text, or JS_UNINITIALIZED where
   there is none. */
JSValue solve_eval_sink_source(JSContext *ctx, JSValueConst handler) {
    JSValue text = JS_UNINITIALIZED;

    solve_eval_sink(ctx, handler);
    if (JS_IsString(handler)) {
        text = JS_DupValue(ctx, handler);
    } else if (concolic_is(handler)) {
        /* A concolic's example is its program text when it is a string, which is what makes
           `setTimeout(cfg.body)` over a loaded config a program this engine runs, as it already does for eval. No
           example (JS_UNDEFINED) or a non-string one holds no program text: step 2's arm. */
        JSValue ex = concolic_example(ctx, handler);

        if (JS_IsString(ex)) text = ex;
        else                 JS_FreeValue(ctx, ex);
    }
    DCHECK(JS_IsUninitialized(text) || JS_IsString(text),
           "the host's string-to-code step answered with something that is not program TEXT and is not the "
           "absence of it — its caller compiles whatever comes back, so a third answer here is a value handed "
           "to a compiler that has no bytes to read");
    /* Raised only where there is a program: an arm that names no bytes compiles nothing, so there is nothing
       downstream for the consumer's assert to cover. */
    g_host_sink_announced = !JS_IsUninitialized(text);
    return text;
}

/* The markup fire oracle's walk. Over the real parse of the sink's output, queue every auto-firing handler
   (`onload`, `onerror`; `onmouseover` needs interaction) as a program of the flow, so X9 fires only if a
   breakout put code in an auto-firing position; innerHTML does not run <script>. It also answers the
   context-escaped rung: a handler attribute whose value carries the marker is an executable position (HTML
   §8.1.1), and the walk returns whether any did. That differs from `fires`, which counts every auto-firing
   handler in the parse, the page's own template included. The walk is `node`'s own subtree through
   node_next_in, which needs no stack, because the tree's depth is the candidate's data. */
static int html_fire_walk(Cand *e, lxb_dom_node_t *node) {
    lxb_dom_node_t *n;
    int at_exec = 0;

    for (n = node; n; n = node_next_in(n, node)) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            static const char *H[] = { "onload", "onerror", NULL };   /* auto-firing only; onmouseover needs interaction */
            for (int h = 0; H[h]; h++) {
                size_t vl = 0;
                const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)H[h], strlen(H[h]), &vl);
                /* Appended: an event handler fires from a task, so it takes the tail like every other task. */
                if (v && vl) {
                    /* The value is a (pointer, length) out of the DOM and is not NUL-terminated, so the marker is
                       searched within its bounds. */
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
/* The markup fire oracle: parse the breakout and run the walk. An oracle may not answer "did not fire" because
   it could not ask, which would be a false negative in the half that must never produce one. The document
   allocation and the parse status are CHECKs: HTML §13.2 tree construction is error-recovering and rejects no
   input, so a non-OK status is memory. A missing document element is a DCHECK, because §13.2.6 inserts html,
   head and body for every input. */
static void html_fire(Cand *e, const char *html) {
    lxb_html_document_t *doc = dom_document_create();
    lxb_dom_element_t *root;
    lxb_status_t st;

    CHECK(doc != NULL, "solve: OOM creating the document an @S markup breakout is fired in — without it the "
                       "oracle reports that the breakout did not fire, which is a false negative in the "
                       "security half rather than a missing measurement");
    /* The parse runs on its own line, outside the assert, so converting the CHECK to a DCHECK cannot delete it. */
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

/* The URL fire oracle: navigating to a `javascript:` URL executes its code, so if the delivered URL's scheme is
   javascript:, the part after the colon is queued as a program of the flow, and X9 fires only if the breakout
   made the URL a javascript: one. */
static void url_fire(Cand *e, JSContext *ctx, const char *url) {
    while (*url == ' ' || *url == '\t' || *url == '\n') url++;   /* leading whitespace is ignored by the URL parser */
    if (!strncasecmp(url, "javascript:", 11)) {
        const char *js = url + 11;
        /* Appended: HTML §7.4.2.2 "Beginning navigation" queues a global task on the navigation and traversal
           task source to navigate to a javascript: URL, so §7.4.2.3.2's evaluation is a task, the position
           engine_queue_javascript_url gives the real one. */
        /* The context-escaped rung for the single-context class: the address the page built from the attacker's
           bytes survived as a javascript: URL. That is still not the fire, which needs the marker to call inside
           the queued evaluation (§7.4.2.3.2 "The javascript: URL special case"). */
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
        /* The half that test cannot say (see solve_eval_sink). It matters most here: this class has no context
           probe, so the marker alone identifies its bytes, and a page that builds a URL containing a minified
           `X9` would otherwise raise the arrival rung and fire its own address. */
        if (!concolic_candidate_delivered()) return;
        /* Converted before the class partition; see solve_eval_sink. */
        if (!(url = JS_ToCString(ctx, arg))) return;
        filter_survived(url);
        if (!(e = candidate_search(SINK_URL))) { JS_FreeCString(ctx, url); return; }   /* another class's search owns this write */
        /* The partition the other two classes make. This class has no probe, so the marker identifies its bytes
           (both CANDS_URL vectors carry X9), and the page's own javascript: hrefs are not queued per candidate. */
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
        /* The half that test cannot say (see solve_eval_sink): "not concolic" is also true of every literal the
           page writes. */
        if (!concolic_candidate_delivered()) return;
        /* Converted before the class partition; see solve_eval_sink. */
        if (!(html = JS_ToCString(ctx, arg))) return;
        filter_survived(html);
        if (!(e = candidate_search(SINK_HTML))) { JS_FreeCString(ctx, html); return; }   /* another class's search owns this write */
        {
            /* Two candidate kinds reach this write, each told apart by bytes the other cannot contain: the context
               probe carries the inert locator and no X9, a derived breakout carries X9 and no locator. Only the
               marker can fire, since html_fire reports only through an auto-firing handler whose code calls X9,
               so a string without it is not parsed at all; otherwise every innerHTML the page writes would be
               parsed once per candidate. */
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
   IT IS A FACT ABOUT THE SEARCH AND NOT ABOUT THE FLOW, which is why the flow does not carry one. The N
   candidates of one sink have ONE root between them BECAUSE cand_learn_root REFUSES a second — which used to be
   a structural fact and is now that assert's job, since a derivation unions its operands' roots and one
   injection identity can reach a sink carrying one root or two. Holding a copy on each Flow would be N owned
   strings that exist only to be asserted equal — plus a dup obligation at every clone, park and free site,
   which is exactly the shape
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
               DERIVATION, never a path. cand_learn_path takes the path for every search at the moments a flow
               stands at the sink — a detection, and this search's own context probe coming back — so there is
               nothing left to select between and a NULL here is a search opened by a door this file does not
               have. */
            {
                DCHECK(e->reinject != NULL,
                       "a candidate is being seeded for a search that holds no re-injection point — both "
                       "doors onto a search freeze the path of the flow standing at its sink before anything "
                       "can be queued against it, so a payload list with no path behind it belongs to an "
                       "entry that acquired breakouts without either a detection or a witness");
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
           this search the thread and this says how far the thread got it.
           AND THAT PAIRING IS WHAT SAYS WHETHER A ZERO RUNWAY MEANS ANYTHING, which is the one question this
           sampling cannot otherwise answer and which nothing else in the document can. The reading is taken
           HERE and at the finish and nowhere between, so a flow that consumes arms and is then switched out
           and never picked again contributes only what its FIRST switch-in saw — which is zero, because a
           flow that has not run has replayed nothing. `tried` counts the candidate FLOWS this search seeded
           and `turns` counts their switch-ins, so `turns > tried` means some flow was switched in twice and
           was therefore sampled AFTER a whole turn of holding the thread: a zero runway beside it is a real
           refusal at the flow's first arm. `turns == tried` means every flow was sampled exactly once, before
           it ran, and the zero is the sampling and not the engine. The two take opposite work and the pair is
           already emitted, so this is a reading a consumer performs rather than a field anyone owes.
           AND THE READING HAS BEEN PERFORMED, WHICH IS WHY THIS IS A NOTE AND NOT AN OPEN QUESTION — but the
           framing above is a DICHOTOMY and the pair has THREE states, which is the half worth keeping. Over
           the terminal `@S` list of every log in the corpus, `turns < tried` is a large minority: more
           candidate flows were seeded than switch-ins were ever counted, so some seeded candidate was never
           handed the thread at all. That is NEITHER branch — such a record mixes samples from flows that ran
           with flows that did not, so a zero over it is not a refusal and is not the sampling either. It does
           not bite here, and that is a measurement rather than a hope: every record carrying a walked of 0
           falls in the `turns > tried` bucket and none falls in `turns == tried`, so the refusal reading is
           the one that holds and the seam this note was written to price is NOT owed. The derivation is
           handed over as a command and not as a count, because the counts move: take the LAST `@S` line of
           each log — never every line, since a census series mixes one candidate before it ran with the same
           candidate after, which is the gauge-versus-lifetime error one directory up — bucket its records by
           `turns` against `tried`, and read `runwayWalked` within each bucket. THE PART THAT DOES NOT MOVE:
           a two-branch pigeonhole over two counters is exhaustive only where one counter cannot exceed the
           other, and nothing here guarantees that.
           IT IS PER-SEARCH AND THAT IS WHY IT IS THE RIGHT ONE. The frontier census's `distMax` looks like it
           answers the same question and cannot: it is a MAX over every member, so one search that walks its
           whole path pins it and no value the others take can move it. Measured on a run reading `distMax`
           0.200 — with FLOW_RUNGS_N at 5 that is a rung sum of exactly 1.0, and a nonzero survival rung needs
           a delivered one which would carry the sum past 2.0, so the only decomposition is a single flow at a
           full replay, which that run had. A frontier-wide max cannot see a per-search under-read. Sampled BEFORE the quantum rather
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
    if (e) {
        observe_runway(e, f);
        /* AND THE FINISH ITSELF, COUNTED HERE BECAUSE THIS IS THE SITE AND THERE IS NO OTHER. `tried` is the
           ASK — candidates SEEDED, raised before one can be picked — and `substituted` is the OUTCOME at the
           source read; this is the third fact neither can carry, and without it a reader cannot tell a
           candidate the path turned away from one still on its way. It is raised at the FINISH and not at the
           switch-out for the reason observe_runway gives about its own sampling: engine.c routes no solve-side
           switch-out seam, and inventing one for a report counter's benefit would be a second door into the
           candidate state.
           THE CHECK HERE USED TO BE `e->ends < e->tried` AND IT WAS A CATEGORY ERROR, WHICH IS RECORDED AT
           LENGTH AT THE FIELD'S DECLARATION AND IN SHORT HERE BECAUSE THIS IS WHERE IT FIRED. `tried` counts
           SEEDS and `ends` counts FLOW FINISHES, and a candidate session is a TREE of flows: engine.c's fork
           copies the whole candidate identity to every sibling, and solve_seed_candidates says a candidate
           forks normally past its recorded cursor because that is where its own exploration begins. So N arms
           of ONE seed each reach this line, and the implication is false at the SECOND arm — by design, not by
           accident.
           ITS ENUMERATION WAS SHORT TWICE, AND THAT IS THE PART TO CARRY RATHER THAN THE ARITHMETIC. It named
           an end that was never seeded and one flow finishing twice. The first miss was its own left-hand
           OPERAND, unwritten at sink_search, which made the check unfalsifiable — six emitted rows carried
           `candEnds` above `tried` while this message occurred 0 times in that run, because the check is HERE
           and the garbage is read at the EMIT. The second miss is a FORKED ARM, which is neither named member:
           two distinct flows each finishing once, both belonging to one seed. An enumeration of the ways an
           assert can fail is not complete until it has asked where each operand comes from AND how many
           objects can produce the event it counts — and the second question is the one a per-flow seam cannot
           answer from the flow in front of it.
           SO WHAT IS ASSERTED IS THE HALF THAT LIVES IN ONE UNIT. Every arm's ancestor was seeded on this same
           entry — both doors raise `tried` before a flow can be picked, and a withdrawn record never reaches
           this line at all because the same refusal drops `cand_src` — so a raise against a search with NO
           seed is the unseeded arrival the old message named first, and it remains impossible. Nothing
           comparing the two counts is assertable here, and the field's declaration says what that costs the
           report.
           AND IT IS A DCHECKF SO A FIRE NAMES ITS OWN ARM, which is what the retired check could not do: the
           values are what separate an unseeded arrival from a search whose seeds are all accounted for, and a
           reader meeting a bare `@WHY` at this line had to guess between them from a log that had not got far
           enough to emit a row. `cand_verifying` is in the list because solve_flow_begin sets it on every
           switch-in and this function clears it, so a 0 here is a flow that has already been through this
           seam — the double-finish arm, separated from the others for free and with no new state.
           THE NUMBERS COME FIRST AND THE PAGE'S OWN STRING LAST, WHICH IS THE ORDER AND NOT A STYLE. The
           composer truncates at a bounded body and appends a marker, and `src` is a SOURCE IDENTITY derived
           from the analysed document — its length is the page's to choose and not this file's to bound. With
           the string first, a long enough source identity would push the diagnostic values off the end of the
           record and the assert would print everything except the reason it exists; with them first, a
           truncation can only ever eat the page's bytes. An unbounded operand goes at the END of any composed
           record whose fixed part is the part being read. */
        DCHECKF(e->tried > 0,
                "an @S candidate flow ended for a search that has seeded none — `tried` is raised by both "
                "doors BEFORE a flow can be picked (solve_seed_candidates at the creation, "
                "solve_resume_candidate during the cold rebuild) and a withdrawn record never reaches this "
                "line because the same refusal drops `cand_src`, so this flow carries a candidate identity "
                "that came from neither door. tried=%d ends=%d resumed=%d withdrawn=%d seeded=%d "
                "payloads=%d turns=%d verifying=%d sink=%s src=\"%s\"",
                e->tried, e->ends, e->resumed, e->resumed_withdrawn, e->seeded, e->npl, e->turns,
                f->cand_verifying,
                f->cand_sink ? f->cand_sink : "(null)", e->src ? e->src : "(null)");
        e->ends++;
    }
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

/* THE SEARCH IS PASSED AS WELL AS ITS ROOT, because the delivery has a half that is a fact about the PATH and
   not about the source: what that path demanded of an attacker's PRINCIPAL. `root` and `delivers` are both
   statements about the source's own carrier and are read off the registry and off the measured table; the gates
   are read off the entry, so the entry comes too. */
static void emit_delivery(JsonBuf *b, const Cand *e, const char *root, const char *delivers) {
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
    /* …AND WHAT THE REPLAYED PATH DEMANDED OF AN ATTACKER'S PRINCIPAL — §Attacker-sources' FORGEABLE half,
       which is the OTHER thing a delivery layer needs and the one it could not previously ask for. `delivery`
       says HOW an attacker puts bytes in this source; this says WHOSE IDENTITY they must hold while doing it,
       and for a `cross-document-message` those are two independent requirements: the post is performable by
       anything that holds a handle, and the victim's handler reads `event.origin` before it reads `event.data`.
       THE VERDICT IS THE FIELD A CONSUMER BRANCHES ON AND IT IS EMITTED FIRST; THESE ROWS ARE ITS EVIDENCE.
       The pair §Attacker-sources states has three examined arms and a fourth state that is not one of them —
       see the `PrincipalDemand` enum for why an ABSENCE here may never be read as "demanded nothing", and for
       the half-deployed artifact that reading would break.
       IT STAYS A SHAPE, exactly as endpoint.c's `predicates` does and in that same grammar so there is ONE
       spelling of a call predicate in this engine's output: the METHOD the page named, the ARGUMENTS it passed,
       and the ARM this run took. Nothing here picks an origin that would satisfy it — §@H forbids inventing `6`
       for `x > 5` and this is the same invention one source kind over — and §@S is where a firing input is
       solved for, by the layer that performs the delivery and can fire-verify what it chose.
       `holds:false` IS A FACT AND NOT A MODIFIER. Forced multi-path runs both arms, so a path that PROVED
       `origin.startsWith("https://admin.")` false is a path whose identity must NOT match it — a constraint on
       the attacker's identity exactly as the true arm is, and the arm the shipped page did not take. */
    if (e && e->pg_demand != PG_UNEXAMINED) {
        /* THE VERDICT FIRST AND UNCONDITIONALLY ON EVERY EXAMINED ENTRY, because it is the field a delivery
           layer BRANCHES on and the one whose absence has to mean "this engine does not state it" rather than
           any answer at all — see the `PrincipalDemand` enum for the half-deployed state that turns on. The
           gates below are its EVIDENCE and are emitted only where there are any. */
        json_buf_raw(b, ","); json_buf_key(b, "principalDemand");
        json_buf_str(b, e->pg_demand == PG_NONE       ? "none"
                      : e->pg_demand == PG_FORGEABLE  ? "forgeable"
                                                      : "unforgeable");
        DCHECK(e->pg_demand == PG_FORGEABLE ? e->npg > 0 : e->npg == 0,
               "a search's principal verdict and its recorded demands disagree — the verdict is DERIVED from "
               "the walk's own answer at the capture, so `forgeable` with no rows would state that an identity "
               "is constrained and not say by what, and rows under any other verdict would offer a delivery "
               "layer a shape to satisfy for a path whose answer is that no shape does");
    }
    if (e && e->npg) {
        json_buf_raw(b, ","); json_buf_key(b, "principalGates"); json_buf_raw(b, "[");
        for (int g = 0; g < e->npg; g++) {
            const PrincipalGate *pg = &e->pg[g];
            if (g) json_buf_raw(b, ",");
            DCHECK(pg->src != NULL && *pg->src && pg->npred > 0 && pg->pred != NULL,
                   "a principal gate reached the emission with no principal named or with no rows — the walk "
                   "that captured it calls back only where it found some, so this row would state that an "
                   "identity is constrained and then not say by what");
            json_buf_raw(b, "{"); json_buf_key(b, "principal"); json_buf_str(b, pg->src);
            json_buf_raw(b, ","); json_buf_key(b, "predicates"); json_buf_raw(b, "[");
            for (int k = 0; k < pg->npred; k++) {
                const ConcolicPred *pr = &pg->pred[k];
                if (k) json_buf_raw(b, ",");
                json_buf_raw(b, "{"); json_buf_key(b, "method"); json_buf_str(b, pr->method);
                json_buf_raw(b, ","); json_buf_key(b, "arguments"); json_buf_raw(b, "[");
                for (int a = 0; a < pr->nargs; a++) {
                    if (a) json_buf_raw(b, ",");
                    json_buf_str(b, pr->args[a]);
                }
                json_buf_raw(b, "]");
                DCHECK(pr->holds == 0 || pr->holds == 1,
                       "a principal gate reached the emission for an arm that is neither taken nor not-taken — "
                       "the arm IS the fact this record carries, so a third value would be written as one of "
                       "the two and the delivery layer could not tell which identity to model");
                json_buf_raw(b, ","); json_buf_key(b, "holds");
                json_buf_raw(b, pr->holds ? "true" : "false");
                json_buf_raw(b, "}");
            }
            json_buf_raw(b, "]}");
        }
        json_buf_raw(b, "]");
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
            /* NO REPORTER, AND THAT IS THE CLAIM RATHER THAN A CONVENIENCE. §4.2.3 and §4.4.1 report a
               violation for content the PAGE ran; this asks the same algorithms about a breakout THIS ENGINE
               composed and has inserted nowhere, to decide whether the finding carries "CSP blocks". No
               content was refused, so there is no violation to report and no document whose listeners should
               see one — firing here would put an event in the page's timeline that names an attack the page
               never suffered. csp_reporter_none is that statement; see core/frame/csp_violation.h. */
            CspReporter no_report = csp_reporter_none();
            int allowed = sc->policy < 0
                              ? policy_allows_string_compilation(no_report, pc)
                              : policy_allows_inline(no_report, pc, (CspInlineType)sc->policy, NULL, poc,
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
            emit_delivery(&b, tw, g_sinks[i].root, cand_delivers(tw, dv, sizeof dv));
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
        /* …AND HOW MANY OF THEM HAVE ENDED, WHICH IS WHAT MAKES EVERY ZERO BELOW READABLE AS A QUESTION ABOUT
           A PATH OR ABOUT THE SCHEDULE. `tried` is the ASK and `turns` is the service; this is the only fact
           here about a candidate's own TERMINATION, and the three are read together:
             `candEnds:0`   beside any zero below — NO FLOW of this search has ended, so nothing it has run
                            got there and nothing in front of the source has been shown to turn anything
                            away. The work is thread rather than a gate to find. This arm is sound and is the
                            load-bearing one.
             `candEnds:N`   at least one flow of this search ended without reaching that point, AND NOTHING
                            MORE. The clause that stood here read `beside tried:N — every candidate this
                            search has ever had ran to its own end`, and it is kept as the reading a reader
                            re-derives because the arithmetic invites it and it is FALSE: this counts FLOW
                            FINISHES and `tried` counts SEEDS, a candidate session is a TREE of flows
                            (engine.c's fork copies the whole candidate identity), so N ends against N seeds
                            can be one candidate that forked N times with every other seed's arms still
                            running. Reading it as `all ended` sends a reader to hunt a gate in front of a
                            source that nothing has been turned away from, which is the confident-wrong
                            direction this whole entry exists to remove.
                            RESIDUAL — WHAT IS NOT COVERED: no reading here can say that a search's candidates
                            are ALL finished, because the pair carries no count of what is still LIVE. WHAT
                            THE NEXT DIFF BUILDS: a per-search live-arm count, raised where the fork copies
                            the candidate identity and lowered at this seam, which with `candEnds` gives
                            `live == 0` as the arm the retired clause was reaching for. HOW ITS ABSENCE SHOWS:
                            a reader holding `candEnds` equal to `tried` cannot distinguish a search whose
                            every candidate is done from one candidate that forked as many times as the search
                            has seeds, and the report renders those two identically.
           UNCONDITIONAL, AND 0 IS THE LOAD-BEARING VALUE: it is the state this engine is in, so an omission
           here would be the defect rather than a statement. Not a rung and not a credit — nothing about the
           WFQ moves at the write.
           AND `it is the state this engine is in` WAS A CLAIM ABOUT ALLOCATOR MEMORY UNTIL THE FIELD WAS
           WRITTEN AT ITS CREATE, which is kept rather than cut because the sentence is right about the DESIGN
           and was wrong about the TREE, and a reader who checks the design finds it sound. sink_search stated
           thirty-four of Cand's thirty-five fields and not this one, so before that repair the `candEnds:0`
           and `candEnds:N` arms above were told apart by whatever the realloc'd slot held: MEASURED in a
           dev-asserts run, 12 of 18 emitted values read 0 and SIX read 3, with six of the eighteen rows
           reading `candEnds` above `tried` outright. The `candEnds:N` arm is the one that would have cost
           something — it tells a reader the readings below are about a PATH, so a garbage value landing on
           `tried` would have sent them to hunt a gate in front of a source nothing had been turned away from.
           AND THE CLAUSE THAT STOOD HERE TOLD A READER TO CHECK THE WRONG THING, WHICH IS RECORDED BECAUSE IT
           IS THE CHEAP CHECK ANYBODY WOULD REACH FOR. It read `candEnds may not exceed tried … the same
           implication the assert at the raise makes`, and BOTH halves went: the assert no longer makes it, and
           it is not true — `candEnds` counts FLOW FINISHES and `tried` counts SEEDS, so a candidate that forks
           legitimately drives this above `tried` and a row doing so is a statement about the SEARCH and not
           about this file. It was a sound reading only while `ends` was garbage, which is the one era in which
           an excess really did mean a defect, and it is exactly the era it was written in.
           WHAT A READER CAN STILL CHECK FOR NOTHING IS ONE-SIDED: a NONZERO `candEnds` beside `tried:0` is
           impossible, because both doors raise `tried` before a flow can be picked and every arm's ancestor
           came through one of them — that is the implication the assert at the raise makes now, in one unit. */
        json_buf_raw(&b, ","); json_buf_key(&b, "candEnds");
        snprintf(t, sizeof t, "%d", g_pending[i].ends); json_buf_raw(&b, t);
        /* …AND THE TWO OBSERVATION COUNTS, WHICH ARE WHAT SPLIT `turns:N,reached:0,survived:0` INTO THE THREE
           STATES IT HAS ALWAYS BEEN. `turns` made `reached:0` readable by separating a search the WFQ has
           never served from one whose flows have run; these separate the second of those into the three things
           it was still saying at once, each taking different work:
             `substituted:0`                      — no run reached its own SOURCE READ. READ WITH `candEnds`,
                                                    which is what says whether they ENDED short of it (a
                                                    question about the PATH, something in front of the source)
                                                    or are still live and have not arrived (a question about
                                                    the SCHEDULE). This clause asserted the first outright and
                                                    is corrected rather than dropped: it is the reading a
                                                    count of arrivals invites, and it names the wrong work.
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
            emit_delivery(&b, &g_pending[i], g_pending[i].root, cand_delivers(&g_pending[i], dv, sizeof dv));
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
        /* THE DEMANDS THAT PATH MADE OF AN ATTACKER'S PRINCIPAL — one owned name and one owned row array per
           principal the path tested, captured beside the path and released with it. The ROWS' own strings are
           concolic.c's to free (gate_pred_free routes to concolic_pred_release); the NAME and the array are
           this file's. */
        for (int c = 0; c < g_pending[i].npg; c++) {
            free(g_pending[i].pg[c].src);
            gate_pred_free(g_pending[i].pg[c].pred, g_pending[i].pg[c].npred);
        }
        free(g_pending[i].pg);
        free(g_pending[i].src);
        free(g_pending[i].root);
    }
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
    for (int i = 0; i < g_sinks_n; i++) { free(g_sinks[i].source); free(g_sinks[i].root); free(g_sinks[i].poc); }
    free(g_sinks); g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
}
