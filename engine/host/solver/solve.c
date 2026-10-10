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
#include "solver/metrics.h"   /* detect_sink raises the arrival census */
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
    METRIC_ADD(sink_reached, 1);
    if (!concolic_is(arg)) return;
    METRIC_ADD(sink_tainted, 1);

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
    if (concolic_principal_pinned()) { METRIC_ADD(sink_suppressed, 1); return; }
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
            static const char *H[] = { "onload", "onerror", NULL };   /* auto-firing; onmouseover needs interaction */
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

/* The delivery root of the search a live candidate belongs to, asked by cold.c when it parks the candidate's
   recipe. It is a fact about the search, not the flow: a search has one root (cand_learn_root refuses a
   second), so a copy on each Flow would be N owned strings to keep equal and to dup at every clone, park and
   free. The park document still writes one per record, because a record is rebuilt alone by a session that
   has nothing else. The search is always there: a candidate exists only because its search was opened, and a
   cold-resumed one re-registers before it runs an opcode. */
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

/* A parked candidate coming back: rebind its sink class by name, raise the search's bookkeeping, and refuse a
   payload this build's carrier contradicts, as one call (solve.h says why). Answers this table's own name
   pointer for the class, or NULL for a withdrawn record. */
const char *solve_resume_candidate(const char *src, const char *root, const char *sink_name,
                                   const char *payload) {
    int i, cls = -1;

    /* The root is part of the identity that crosses: a resumed candidate opens its search here rather than at a
       detection, so a record without one would emit an envelope saying no component carries these bytes. */
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
    /* Find-or-create, not add_pending: an earlier session opened this search and the flow being rebuilt is one of
       its candidates, so opening it again would seed the whole search a second time. `tried` rises once per
       record accepted, one candidate run each; a refused record raises `resumed_withdrawn` instead. */
    {
        int created = 0;
        Cand *e = sink_search(src, cls, &created);
        /* The root goes on before the entry is visible to any reader. On a later record for the same sink this
           is cand_learn_root's equality assert, the one check that a park document's records agree. */
        cand_learn_root(e, root);
        /* And the payload is refused here, at the first instant it can be asked, because cand_learn_root has just
           seeded the carrier half of the table. It is asked with solve_delivered_ok, the predicate
           solve_seed_candidates asks of a derived escape, so both doors share one refusal. The premise this
           falsifies is that these bytes reach the sink as themselves: the root's carrier says one of them never
           enters the program in any form (for a cookie, RFC 6265 §4.1.1's cookie-octet excludes it), so no run
           can widen it. It is pruning, not a cap: there is no count, age or seen-set, and the world survives,
           since cold.c drops the candidate fields and the flow comes back as an exploration flow that replays to
           this sink, detects, and re-derives against the narrowed table. No probe can be refused here: both
           probes are printable US-ASCII by construction, the refusal covers only bytes outside it, and
           cand_learn_root holds the declared and refused sets disjoint. Only the withdrawal is counted, because
           no run is spent. */
        if (!solve_delivered_ok(&e->deliv, payload)) {
            e->resumed_withdrawn++;
            return NULL;
        }
        e->tried++;
        /* And which of those runs this is: the term of solve.h's `tried` arithmetic that has no row in `pl`. */
        e->resumed++;
    }
    /* A resumed candidate re-runs the whole page as a fresh one does, so it counts as one. */
    g_cands_seeded++;
    return SINKS[cls].name;
}

/* Seed a candidate flow for every candidate past each search's cursor, and answer how many were added. Each is
   an ordinary member of the one frontier: the candidate is injected at the source, the real program re-runs
   as a flow the scheduler preempts and parks like any other, and the marker firing is the replay-verified
   PoC. Idempotent, so the scheduler asks again whenever the frontier drains: a derived breakout is appended
   after its probe has run and a sink in late-loaded code is found after the first drain, and the cursor
   picks both up without seeding anything twice. */
int solve_seed_candidates(JSContext *ctx) {
    int added = 0;
    for (int i = 0; i < g_pending_n; i++) {
        Cand *e = &g_pending[i];
        /* Asked again at every drain, because between a breakout's append and its seeding another candidate of
           the same search can fire, close it, and release the path the install below reads. */
        if (!search_seeds(e)) continue;
        for (; e->seeded < e->npl; e->seeded++) {
            Flow *f;
            /* Withdrawn: this search's own measurement has since contradicted this spelling, so it does not
               become a flow. queue_derived asks deliverability at push time and the table narrows afterwards
               (observe_delivery), so an escape built while everything delivered can stay in `pl` after its
               permission is gone; seeded, it would cost a whole document re-run that can only arrive
               transformed. This prunes a contradicted arm and is not a bound: the one question is whether
               something positive (a delivery run, or the root's carrier declaration) contradicts these bytes
               arriving, and the table never widens, so a withdrawal is permanent and the cursor stays a cursor.
               The entry stays in `pl`, so the report can say an escape was built and withdrawn (`withdrawn`).
               Probes are never withdrawn: the delivery probe is built from the very bytes in question and the
               context probe is ASCII alphanumeric, so both are measurements rather than attacks. */
            if (e->pl[e->seeded].kind == CAND_ESCAPE &&
                !solve_delivered_ok(&e->deliv, e->pl[e->seeded].bytes)) continue;
            f = flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* a candidate session runs from the baseline */
            f->cand_src     = strdup(e->src);
            f->cand_payload = strdup(e->pl[e->seeded].bytes);
            f->cand_sink    = sink_name(e->sink);
            CHECK(f->cand_src && f->cand_payload, "solve: OOM seeding a candidate flow");
            /* The other side of the withdrawal, asserted where the cost is taken: this is where a payload becomes
               a document re-run, so a route into flow creation that skips the check above crashes here instead
               of quietly spending the run. */
            DCHECK(e->pl[e->seeded].kind == CAND_PROBE || solve_delivered_ok(&e->deliv, f->cand_payload),
                   "a candidate flow was created for a payload this search has OBSERVED cannot reach its sink — "
                   "the delivery table narrows after a breakout is queued, so deliverability asked once at the "
                   "derivation is a constraint that has expired by the time the flow is made, and this run can "
                   "only end with the candidate arriving transformed at its own sink");
            /* And the flow stands on the path the detection proved reaches this sink. It still re-runs the
               document from the baseline (the payload enters at the source read), but it consumes the recorded
               arms at each branch it re-reaches instead of forking over them. decide_blob_new takes its own
               reference on the frozen segment and replays at cursor 0, forking normally once the cursor runs past
               what the detecting flow knew, which is where this candidate's own exploration begins. It is
               unconditional: every class needs a path, and both doors onto a search take one before anything can
               be queued against it. */
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
                /* The triple is one install. A flow on a recorded path must be `started`, because flow_switch_in
                   routes on that bit and a never-run flow takes decide_enter, which replays from nothing and never
                   reads `dec_blob`; the blob alone would be ignored, then leaked with its segment reference when
                   engine.c overwrites `dec_blob` at the first suspend. The empty pin blob is the third member
                   because flow_switch_in's resume branch calls concolic_pins_resume beside decide_resume, and a
                   replaying flow re-derives every pin from the gates it replays. cold.c's 'f' and 'c' arms install
                   the same triple for a cold-resumed candidate. */
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

/* This search's bytes just entered the page's own program: the report's bottom rung, written from the one site
   that can state it, the source read, which is strictly before every sink. It is the report's copy of the
   event flow_observe_rung records as FLOW_RUNG_DELIVERED, and the two are separate quantities: the flow's rung
   is the comparator (monotone, per flow, re-earned across a park), while this is a count on the search that
   orders nothing and outlives its flows. No credit and no crossing: paid as a ledger quantity it would reward
   only a search's first delivery, which teaches nothing the second does not. CHECKs, because the flow's
   source and sink are dereferenced immediately. */
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
    /* The link below `substituted`: a substitution happens at a candidate's own source read, which a flow reaches
       only while it holds the thread, the switch-in solve_flow_begin counts. So `substituted:D` over `turns:0` is
       `turns` counting something other than this search's flows. Written out rather than shared with
       learn_witness's identical assert, because a DCHECK stamps the line it is written at. */
    DCHECK(e->turns > 0,
           "an @S candidate performed its substitution for a search the scheduler reports it has never given "
           "a turn to — solve_flow_begin raises `turns` at every switch-in of a candidate flow, and a flow "
           "reaches its own source read only while it holds the thread, so `turns` is not counting the flows "
           "that do this search's work and `turns:0` would be read as WFQ starvation for a search that ran");
    e->substituted++;
}

/* Take the runway reading at the two seams this file has: the switch-in (solve_flow_begin, where `turns` is
   counted) and the finish (solve_flow_end). Flow.cand_replay moves inside dec_replay arm by arm, which this
   file neither calls nor is called back from; the ratchet makes sparse samples add up to the search's furthest,
   since the per-flow value only rises. It reads the field and not flow_distance, which is the whole comparator,
   so this file does not re-derive another component's arithmetic.
   Named residual: not covered — the runway a candidate walks after its last switch-in when it is then parked or
   still live at emission, since engine.c routes no solve-side switch-out seam; next diff — take the reading
   where flow.c already has it, flow_observe_replay(Flow *, long consumed, long total); absence shows as a
   `runwayPerMille` that under-reads a search whose flows are long-lived and rarely switched, never one that
   over-reads. */
static void observe_runway(Cand *e, const Flow *f) {
    int pm;

    DCHECK(f->cand_replay >= 0.0 && f->cand_replay <= 1.0,
           "an @S candidate's runway position is not a fraction of its own recorded path — flow.c's "
           "flow_observe_replay asserts [0,1] at every write and flow_observe_rung pins it to exactly 1.0 at "
           "the delivery, so a value outside that range is the ladder's bottom rung carrying something other "
           "than the fraction this record is about to publish as one");
    pm = (int)(f->cand_replay * 1000.0 + 0.5);
    if (pm > e->replay_pm) e->replay_pm = pm;
    /* And the pair, ratcheted on itself: `pm` is rounded, so riding its condition would drop a best of 3/8000.
       Cross-multiplied, so the comparison is exact and no float is stored: with both denominators positive,
       a/b > c/d is a*d > c*b. `of == 0` means no reading yet on either side, and is tested rather than left to
       make a product of zero decide. */
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

/* Install the incoming flow's candidate state on every switch-in, unconditionally: a flow with no candidate
   installs "no candidate". The substitution mirrors the running flow rather than being a bracket someone opens
   and closes, so a candidate's payload and endpoint suppression cannot leak into an ordinary flow scheduled
   after it, which would read the attacker's concrete string where its concolic source belongs. */
void solve_flow_begin(Flow *f) {
    concolic_set_candidate(f ? f->cand_src : NULL, f ? f->cand_payload : NULL);
    endpoint_suppress(f && f->cand_src ? 1 : 0);
    if (f && f->cand_src) {
        f->cand_verifying = 1;
        /* The search is given a turn, counted at the one point a candidate of it is about to execute. Beside
           `tried` (seeded) and `reached` (arrived) it separates a search the WFQ has never served from one whose
           flows ran and fell short of the sink. It counts switch-ins, not distinct flows: a candidate preempted
           and resumed twenty times has had twenty turns. */
        Cand *e = search_of(f->cand_src, sink_class_of_name(f->cand_sink));
        DCHECK(e != NULL,
               "a candidate flow was switched in for a sink search this session has no entry for — a candidate "
               "exists only because detection opened one, and a cold-resumed one re-registers before it runs "
               "(solve_resume_candidate), so an absent entry means the search was dropped under a live flow");
        /* The link below this rung: both doors raise `tried` before a flow can be picked (solve_seed_candidates at
           creation, solve_resume_candidate during the cold rebuild), so a switch-in over `tried:0` is a third door.
           A withdrawn record is no exception: the same refusal makes cold.c drop `cand_src`, so it never reaches
           here as a candidate. */
        DCHECK(!e || e->tried > 0,
               "an @S candidate flow was switched in for a search that reports no candidate seeded — both "
               "doors into a candidate raise `tried` before the flow is reachable by the pick, so a turn "
               "standing over `tried:0` is a candidate assembled outside them and the parked card is about "
               "to report turns with nothing behind them");
        if (e) e->turns++;
        /* And the runway this candidate had walked when it last held the thread, taken with `turns` because the two
           are one reading: `turns` says the WFQ gave this search the thread, this says how far it got. Samples are
           taken here and at the finish only, so a flow switched in once contributes the zero it had before it ran.
           Read the zero against `turns` and `tried` (both emitted), on a log's last @S line rather than on every
           census line: `turns > tried` means some flow was sampled after a whole turn, so a zero beside it is a
           refusal at the first arm; `turns == tried` means every flow was sampled before it ran; `turns < tried`
           means some seeded candidate never ran, and that mix is neither. The frontier census's `distMax` cannot
           answer this: it is a max over every member, so one search at a full replay pins it. Sampled before the
           quantum, so it states runs that have happened. */
        if (e) observe_runway(e, f);
    }
}

/* A candidate flow finished. The fire is recorded at the marker (js_x9), where the proof happens, and not here,
   because a flow owes nobody completion. The globals are not cleared: the next switch-in installs the next
   flow's state, and clearing in two places is how an asymmetry gets in. This file no longer sets
   Flow.cand_fired, so engine.c's sibling copy of it and cold.c's drop of it carry nothing. */
void solve_flow_end(Flow *f) {
    if (!f || !f->cand_src) return;
    /* The last runway reading this candidate will offer, which switch-in sampling cannot take for a flow that
       holds the thread from its final switch-in to its end. Taken before `cand_verifying` is cleared. */
    Cand *e = search_of(f->cand_src, sink_class_of_name(f->cand_sink));
    DCHECK(e != NULL,
           "a candidate flow ended for a sink search this session has no entry for — a candidate exists only "
           "because detection opened one and a cold-resumed one re-registers before it runs an opcode, so an "
           "absent entry here is the search having been dropped under a flow that was still running it, and "
           "the runway this candidate walked is about to be lost with it");
    if (e) {
        observe_runway(e, f);
        /* And the finish itself, counted here because this is the only site: `tried` is the ask and `substituted`
           the outcome at the source read, and only this tells a candidate the path turned away from one still on
           its way. `ends` counts flow finishes and `tried` counts seeds, and a candidate session is a tree of
           flows whose arms each finish here, so no comparison of the two is assertable; what is asserted is the
           one-unit half, that a seed exists. A withdrawn record never reaches this line, because the same refusal
           drops `cand_src`. It is a DCHECKF so a failure names its arm: `cand_verifying` 0 is a flow that already
           passed this seam, a double finish. The numbers come first and the page-chosen source identity last,
           because the composer truncates a bounded body and only the page's bytes may be lost. */
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

/* ── @S JSON emit, written with core/json_buf.h ── Does (class, source) have a fire-verified finding? The
   parked emit skips those. */
static int solved(int cls, const char *src) {
    for (int i = 0; i < g_sinks_n; i++)
        if (g_sinks[i].cls == cls && !strcmp(g_sinks[i].source, src)) return 1;
    return 0;
}

/* Which declared bytes a run actually saw arrive: the measured half of the constraint, beside the declared one.
   Equal sets mean the page decodes its own fragment and every escape is open; an empty set beside a full
   declaration means the source's transform is the whole failure. Answers NULL where there is nothing to say: a
   source with no percent-encode set has no byte in question, and a search whose delivery probe has found no
   token has taken no measurement, so the permissive initial table is never emitted as one. */
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

/* The source's declared browser delivery, written the same way into both entry shapes: `sourceEncodes` is what a
   breakout had to survive and `sourceDelivers` what a run measured, while `delivery` and `deliveryPrefix` are
   what a reproduction must perform. Asked with the root, not the injection identity, because the registry
   matches only declared sources. An undeclared source writes nothing, and that silence is a fact
   (server-injected page state is written by the attacker directly); a missing root is not that fact, which is
   why it is asserted. The search comes too, for what its path demanded of an attacker's principal. */
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
    /* Immediately after the declaration it is a subset of, so the pair reads as one statement. */
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
    /* What the replayed path demanded of an attacker's principal. `delivery` says how an attacker puts bytes in
       this source; this says whose identity they must hold while doing it. For a cross-document message the post
       is performable by anything holding a handle, and the victim's handler reads `event.origin` before
       `event.data`. The verdict is emitted first and the rows are its evidence (see PrincipalDemand). The rows
       stay a shape, in endpoint.c's `predicates` grammar: method, arguments, and the arm this run took. Nothing
       here picks an origin that satisfies them; the delivery layer solves for one and fire-verifies it.
       `holds:false` is a fact: a path that proved the predicate false demands an identity that fails it. */
    if (e && e->pg_demand != PG_UNEXAMINED) {
        /* The verdict on every examined entry, unconditionally, because its absence must mean "not stated". */
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

/* Every detected sink is reported: the fire-verified PoCs, and every search that has not solved, as a parked
   search (reached, searched this far, not broken out of yet). Omitting the unsolved ones would read as "no
   attacker input gets here", the safe verdict solve.h forbids, arrived at by omission. A parked entry carries
   how far its search got and the constraint every candidate had to survive; for `innerHTML` fed from
   `location.hash` the fragment set contains `<`, which is a constraint and not a claim that the sink is safe,
   since a page that decodes its fragment breaks out with the same candidate. */
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
        /* Every PoC carries its reproduction envelope, starting with what makes it run, from the sink's own fire
           oracle (see SINKS). */
        json_buf_raw(&b, ","); json_buf_key(&b, "firesOn"); json_buf_str(&b, sc->fires_on);
        /* A firing breakout in the model is not yet a working exploit: it must run under the page's actual policy,
           and an inline `onerror` is dead under `script-src 'self'`. The finding stays and carries what blocks it
           ("sink real, CSP blocks"). The PoC is the `source` §6.7.3.3 is asked about, with a NULL element because
           the breakout is inserted nowhere, the shape §4.2.4 uses for a javascript: navigation; that keeps the
           nonce arm out, as attacker markup cannot carry a nonce the page's policy lists. */
        {
            const char *poc = g_sinks[i].poc;
            /* No reporter: §4.2.3 and §4.4.1 report a violation for content the page ran, and this asks them about
               a breakout this engine composed and inserted nowhere, so no event belongs in the page's timeline
               (csp_reporter_none; see core/frame/csp_violation.h). */
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
        /* The same rule one algorithm earlier: under `require-trusted-types-for 'script'` an innerHTML assignment
           throws before the markup is parsed. The value is the sink group the CSP names, which is what a reader
           adds a policy for. */
        if (sc->tt >= 0 && trusted_types_required(ctx, (TrustedTypeKind)sc->tt)) {
            json_buf_raw(&b, ","); json_buf_key(&b, "trustedTypes");
            json_buf_str(&b, "script");
        }
        /* The delivery, including whether this is a two-stage plant-then-load PoC: a `plant` delivery is two-stage
           and every other is a single load, so no separate `stored` field states the same fact twice.
           And what the solve cost, because a fired search stops emitting the parked shape and its counts vanish
           the moment they describe a real exploit. `searched` is `tried`, the whole search's cost in document
           re-runs, which tells a PoC from the first written-down vector from one that took forty runs and a
           derivation. A twin always exists, because record_sink CHECKs it at the store. */
        {
            const Cand *tw = search_of(g_sinks[i].source, g_sinks[i].cls);
            char t[32], dv[64];
            CHECK(tw != NULL,
                  "solve: a fire-verified @S finding is being emitted with no search behind it — record_sink "
                  "asserts the twin at the moment the PoC is stored, so an absent one here means the pending "
                  "list was rewritten under a finding and the report is about to state a cost it cannot read");
            json_buf_raw(&b, ","); json_buf_key(&b, "searched");
            snprintf(t, sizeof t, "%d", tw->tried); json_buf_raw(&b, t);
            /* And the measured constraint the PoC was built under, which tells anyone reproducing it by hand which
               bytes the browser would have eaten. */
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
        /* `resumed`: how many of those runs came back from a previous session, the term of solve.h's `tried`
           arithmetic that has no row in `payloads`. It is what makes `reached` readable beside a probes-only
           list, so the consumer states it from this number instead of re-deriving it. Unconditional; 0 says every
           run this search has had is a row below. */
        json_buf_raw(&b, ","); json_buf_key(&b, "resumed");
        snprintf(t, sizeof t, "%d", g_pending[i].resumed); json_buf_raw(&b, t);
        /* `resumedWithdrawn`: how many came back and were refused before they could run, because this build's
           carrier declaration contradicts their payload (solve_resume_candidate). Neither `tried` nor `resumed`
           moves for them, so without this a search whose every parked candidate was refused reads like one that
           nothing was parked for. Unconditional; 0 says the residue and this build agree about the source. */
        json_buf_raw(&b, ","); json_buf_key(&b, "resumedWithdrawn");
        snprintf(t, sizeof t, "%d", g_pending[i].resumed_withdrawn); json_buf_raw(&b, t);
        /* `reached`: how many runs got here, which `tried` cannot state; `reached:0` is a document not explored
           far enough, anything else a breakout that arrived and did not work. */
        json_buf_raw(&b, ","); json_buf_key(&b, "reached");
        snprintf(t, sizeof t, "%d", g_pending[i].reached); json_buf_raw(&b, t);
        /* `turns`: at `turns:0` this search's candidates have never held the thread, and at `turns:N` they ran and
           did not reach the sink, a WFQ question against a distance question. */
        json_buf_raw(&b, ","); json_buf_key(&b, "turns");
        snprintf(t, sizeof t, "%d", g_pending[i].turns); json_buf_raw(&b, t);
        /* `candEnds`: how many of this search's flows have ended, which makes every zero below readable as a
           question about a path or about the schedule. `candEnds:0` beside a zero below means no flow has ended,
           so nothing was shown to be turned away and the work is thread. `candEnds:N` means at least one flow
           ended short of that point, and no more: it counts flow finishes while `tried` counts seeds, so N ends
           against N seeds can be one candidate that forked N times. Only `candEnds > 0` beside `tried:0` is
           impossible. Unconditional; 0 is the load-bearing value. Not a rung and not a credit.
           Named residual: not covered — whether all of a search's candidates have finished, since nothing
           counts what is still live; next diff — a per-search live-arm count raised where the fork copies the
           candidate identity and lowered at solve_flow_end, so `live == 0` gives that reading; absence shows as
           `candEnds` equal to `tried` reading the same for a finished search and for one candidate that forked
           as often as the search has seeds. */
        json_buf_raw(&b, ","); json_buf_key(&b, "candEnds");
        snprintf(t, sizeof t, "%d", g_pending[i].ends); json_buf_raw(&b, t);
        /* `substituted` and `sinkStrings` split `turns:N,reached:0,survived:0` into the states it was saying at
           once:
             `substituted:0`                — no run reached its own source read; `candEnds` says whether they
                                              ended short of it (a path question) or are still live (a schedule
                                              question);
             `substituted:D, sinkStrings:0` — the bytes entered the program and no code-execution sink ran while
                                              they were live: the distance question;
             `sinkStrings:S, survived:0`    — S sinks ran and none of the payload was in their strings: the
                                              payload's own transform or routing.
           Both are unconditional, 0 being a real value. `sinkStrings` counts strings handed to any sink, not
           arrivals: `sinkStrings:400` is not four hundred arrivals. */
        json_buf_raw(&b, ","); json_buf_key(&b, "substituted");
        snprintf(t, sizeof t, "%d", g_pending[i].substituted); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "sinkStrings");
        snprintf(t, sizeof t, "%d", g_pending[i].sink_strings); json_buf_raw(&b, t);
        /* `runwayPerMille` splits `substituted:0` by the approach to the source read: 0 means the candidates
           consumed none of their recorded path (what turns a replay back at its first arm), ~1000 that they
           consumed all of it and the source read is still ahead (the distance question, with the fitness rung
           below FLOW_RUNG_DELIVERED saturated). Thousandths, unit in the key. Unconditional: a 0 beside `turns:0`
           is candidates never switched in, beside `turns:N` candidates that replayed nothing, and beside
           `runwayArms:0` a detection that decided no branch, a tautology. */
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayPerMille");
        snprintf(t, sizeof t, "%d", g_pending[i].replay_pm); json_buf_raw(&b, t);
        /* `runwayArms`, emitted beside the fraction because its value is the joint reading (see the field); it is
           not the fraction's denominator. */
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayArms");
        snprintf(t, sizeof t, "%d", g_pending[i].reinject_len); json_buf_raw(&b, t);
        /* `runwayWalked`/`runwayOf`: the position as its two halves, which the thousandths round away.
           `runwayOf` is dec_total() when the reading was taken, not `runwayArms`. Below the delivery
           runwayPerMille == round(runwayWalked/runwayOf*1000); past it the thousandths carry flow_observe_rung's
           pin and the pair never does. */
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayWalked");
        snprintf(t, sizeof t, "%ld", g_pending[i].replay_arms); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "runwayOf");
        snprintf(t, sizeof t, "%ld", g_pending[i].replay_of); json_buf_raw(&b, t);
        /* `survived`/`survivedOf`: the furthest any candidate of this search has got its own bytes through the
           page's transforms to any sink, so `turns:900,reached:0,survived:11,survivedOf:14` is a filter eating the
           candidate eleven-fourteenths of the way in. A best-so-far is silent about how often it looked, so a 0 is
           read with `substituted` and `sinkStrings`. `escaped` is how many arrivals reached an executable
           position, which `fires` only approximates, since `fires` also counts the page's own handlers. Both are
           unconditional; `survivedOf:0` beside `survived:0` says once that nothing was observed. */
        json_buf_raw(&b, ","); json_buf_key(&b, "survived");
        snprintf(t, sizeof t, "%d", g_pending[i].surv_run); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "survivedOf");
        snprintf(t, sizeof t, "%d", g_pending[i].surv_len); json_buf_raw(&b, t);
        /* `survivedAt`/`survivedTo`: which segment lived and where it landed, the input to a near-miss mutation.
           `survivedAt` is the run's offset into the candidate and `survivedTo` its offset in the sink's string;
           a run at offset 0 is a payload whose tail the page cut, a later offset one whose head it ate. Absent
           when no run is recorded, decided on the run, because 0 is a real and common offset. */
        if (g_pending[i].surv_run > 0) {
            json_buf_raw(&b, ","); json_buf_key(&b, "survivedAt");
            snprintf(t, sizeof t, "%d", g_pending[i].surv_at); json_buf_raw(&b, t);
            json_buf_raw(&b, ","); json_buf_key(&b, "survivedTo");
            snprintf(t, sizeof t, "%d", g_pending[i].surv_out); json_buf_raw(&b, t);
        }
        json_buf_raw(&b, ","); json_buf_key(&b, "escaped");
        snprintf(t, sizeof t, "%d", g_pending[i].escaped); json_buf_raw(&b, t);
        /* `fires`, for the classes that queue one. Arriving is not reaching an executable position: html_fire
           queues a program only if the parse put the marker in an auto-firing handler, and url_fire only for a
           surviving javascript: URL. So `reached:1,fires:0` says the source's transform defeated this breakout,
           and `reached:1,fires:1` that a program exists and has not run yet, which belongs to the flow's
           sequence. Absent for an eval sink, which evaluates its own argument and queues nothing. */
        if (sink_class(g_pending[i].sink)->queues_fire) {
            json_buf_raw(&b, ","); json_buf_key(&b, "fires");
            snprintf(t, sizeof t, "%d", g_pending[i].fires); json_buf_raw(&b, t);
        }
        /* `witnessed`: how many distinct sink writes the context probe reached. A derived class builds nothing
           until its probe arrives, so `probes == payloads` is either a probe that has not reached the sink (a
           distance question) or one that did, in a state no escape could be constructed from, which for a
           percent-encoded source is the correct final answer. It counts distinct writes (learn_witness dedups
           by text), so `witnessed:2` is two contexts read. Absent for a single-context class, which runs no
           probe. */
        if (sink_class(g_pending[i].sink)->derive != SINK_DERIVE_NONE) {
            json_buf_raw(&b, ","); json_buf_key(&b, "witnessed");
            snprintf(t, sizeof t, "%d", g_pending[i].nwit); json_buf_raw(&b, t);
        }
        /* `deliveryProbed`: how many times the delivery probe reached a sink. Without it an absent `sourceDelivers`
           says both "the probe has not run" (wait for the scheduler) and "it ran and the page destroyed every
           token" (stop deriving: the strongest thing a parked search can report). It does not move
           `sourceDelivers`' gate, which stays on `deliv_seen`, because a table narrowed by nothing is the
           permissive one and would claim every declared byte arrives. Absent for a search with no delivery probe
           (cand_has_delivery_probe). */
        if (cand_has_delivery_probe(&g_pending[i])) {
            DCHECK(!g_pending[i].deliv_seen || g_pending[i].deliv_runs > 0,
                   "an @S search reports a delivered byte observed while its delivery probe has never reached "
                   "a sink — observe_delivery counts the arrival before it looks for a single token, so a byte "
                   "learned without one is a second door into the delivery table, and `sourceDelivers` would "
                   "be a constraint no run of this search measured");
            json_buf_raw(&b, ","); json_buf_key(&b, "deliveryProbed");
            snprintf(t, sizeof t, "%d", g_pending[i].deliv_runs); json_buf_raw(&b, t);
        }
        /* `probes`: how many of `payloads`' entries are probes, from their labels. `payloads.length > probes` is
           "this search has constructed an escape" (cand_has_escape); the consumer must not re-derive it, since the
           marker vocabulary is this engine's. Without it an `innerHTML` sink fed the raw fragment, which seeds two
           probes and no escape because no escape byte arrives, would be described as a breakout that has not
           traversed the document or was filtered. Unconditional: `probes:0` beside a non-empty list says every
           entry is an attack.
           `payloads` is what was tried, as the search built it and never as the browser delivers it (the source's
           transform is stated once, as `sourceEncodes` and `deliveryPrefix`). A derived class's probes are listed
           like the rest, because they are runs `tried` counts; they carry no marker and cannot fire. */
        json_buf_raw(&b, ","); json_buf_key(&b, "probes");
        snprintf(t, sizeof t, "%d", cand_probes(&g_pending[i])); json_buf_raw(&b, t);
        json_buf_raw(&b, ","); json_buf_key(&b, "payloads"); json_buf_raw(&b, "[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_raw(&b, ",");
            json_buf_str(&b, g_pending[i].pl[c].bytes);
        }
        json_buf_raw(&b, "]");
        /* `survivedBy`: how far each payload got, index-aligned with `payloads`. `survived` saturates once any
           candidate lands intact, so only this says which: `[14,0]` is a probe whose bytes reached a sink and a
           breakout that never did, `[14,9]` one where the breakout travelled too and something after arrival is
           the problem. */
        json_buf_raw(&b, ","); json_buf_key(&b, "survivedBy"); json_buf_raw(&b, "[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_raw(&b, ",");
            snprintf(t, sizeof t, "%d", g_pending[i].pl[c].surv); json_buf_raw(&b, t);
        }
        json_buf_raw(&b, "]");
        /* `withdrawn`: which payloads the search's own measurement has since withdrawn, index-aligned. A withdrawn
           entry is never seeded, so it never raises `tried` and its `survivedBy` stays 0, which would otherwise
           read exactly like a breakout that ran and got nowhere, the opposite verdict. It also reconciles `tried`
           with `payloads` in solve.h's arithmetic. The producer states it because the table is held on the
           search, out of the consumer's reach. */
        json_buf_raw(&b, ","); json_buf_key(&b, "withdrawn"); json_buf_raw(&b, "[");
        for (int c = 0; c < g_pending[i].npl; c++) {
            if (c) json_buf_raw(&b, ",");
            json_buf_raw(&b, (g_pending[i].pl[c].kind == CAND_ESCAPE &&
                               !solve_delivered_ok(&g_pending[i].deliv, g_pending[i].pl[c].bytes)) ? "1" : "0");
        }
        json_buf_raw(&b, "]");
        /* A parked entry carries the declaration, not the envelope: an unsolved search has no PoC for `firesOn`,
           `cspBlocks` or `trustedTypes` to describe. It carries the bytes a candidate must survive and how an
           attacker would reach the victim if one fires. */
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
    /* The eval seam is given back first: the engine announces every evaluation while a hook is installed, and
       everything below frees the store add_pending writes into, so a later eval would detect into freed memory.
       NULL is the registration's word for "no host is listening". */
    JS_SetEvalSinkHook(NULL);
    for (int i = 0; i < g_pending_n; i++) {
        for (int c = 0; c < g_pending[i].npl; c++) free(g_pending[i].pl[c].bytes);
        free(g_pending[i].pl);
        /* The context witnesses: one owned copy per distinct sink write, living as long as the search. */
        for (int c = 0; c < g_pending[i].nwit; c++) free(g_pending[i].wit[c]);
        free(g_pending[i].wit);
        /* The segment reference an unsolved search still holds; the frozen chain goes with its last reference. */
        if (g_pending[i].reinject) decide_blob_free(g_pending[i].reinject);
        /* The principal demands: this file owns each name and row array, and the rows' strings are concolic.c's
           (gate_pred_free routes to concolic_pred_release). */
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
