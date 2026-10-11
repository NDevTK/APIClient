/* The rest-point granularity policy (see solver/rest_unit.h): one number per kind and the reason for it.
   Components ask; none keeps its own copy, so two components filling the same stream cannot disagree about
   the unit — core/loader/html_document.c and core/loader/text_document.c both read REST_UNIT_INPUT_BYTES. */
#include "solver/rest_unit.h"

#include "solver/engine.h"     /* ENGINE_QUANTUM_MS: the ceiling is the scheduler's slice */
#include "solver/quantum.h"    /* the consumed-CPU reading, and whether this host's reading is CPU */
#include "check.h"

/* HTML §7.5.2 "Loading HTML documents" / §7.5.4 "Loading text documents": one networking task's bytes. The
   standard names the unit but cannot size it, and this engine's transport hands the loader a whole decoded
   entity, so the policy states one. The floor is the cost of asking (a yield, a re-rank, possibly a delta
   swap), paid once per 16 KiB and invisible beside the parse. The ceiling is bound (2): 16 KiB of
   tokenization and tree construction is far under one slice, and the bracket asserts it. Between the two
   the value is free. */
#define REST_UNIT_INPUT_BYTES_N   16384u

/* HTML §7.5.3 "Loading XML documents": one construct. A construct's size is the document's choice ([14]
   `CharData` is unbounded), so a multiplier would multiply a quantity the page picks and break bound (1).
   One construct already amortizes the cost of asking. A construct that overruns the slice means core/xml/'s
   [14] scan must become a pull itself, not that this number is wrong. */
#define REST_UNIT_XML_CONSTRUCTS_N   1u

/* §7.5.3's discard of a failed parse's partial tree: one top-level child, for the construct's reason —
   removing a child removes a subtree the document chose. */
#define REST_UNIT_SUBTREE_REMOVALS_N   1u

/* Layout question bodies above one drive before an ask is deferred. The floor is replay: deferring bodies
   re-run from their start, so a unit of one replays a parent once per child. The ceiling is the C stack one
   activation spends. 32 replays a body once per 32 levels and keeps an activation to 32 bodies. */
#define REST_UNIT_LAYOUT_QUESTIONS_N   32u

/* Layout question bodies a resting drive opens per rest point, 8. The floor is replay: each body that
   deferred re-runs from its start in the next activation. The ceiling is bound (2), measured by the
   scrolling-area bracket the drive runs inside; a cold node's bodies run to a few hundred microseconds each. */
#define REST_UNIT_LAYOUT_BODIES_N   8u

/* Descendants folded into a scrolling area per rest point. The floor is the cost of asking, which one
   node's edge does not amortize; the ceiling is bound (2), asserted around each unit. A node whose
   derivation overruns the slice rests mid-node through its job's resting drive (core/layout/layout_job.h). */
#define REST_UNIT_LAYOUT_DESCENDANTS_N   32u

size_t rest_unit_items(RestUnitKind kind)
{
    size_t n = 0;

    switch (kind) {
    case REST_UNIT_INPUT_BYTES:      n = REST_UNIT_INPUT_BYTES_N;      break;
    case REST_UNIT_XML_CONSTRUCTS:   n = REST_UNIT_XML_CONSTRUCTS_N;   break;
    case REST_UNIT_SUBTREE_REMOVALS: n = REST_UNIT_SUBTREE_REMOVALS_N; break;
    case REST_UNIT_LAYOUT_QUESTIONS: n = REST_UNIT_LAYOUT_QUESTIONS_N; break;
    case REST_UNIT_LAYOUT_BODIES:    n = REST_UNIT_LAYOUT_BODIES_N;    break;
    case REST_UNIT_LAYOUT_DESCENDANTS: n = REST_UNIT_LAYOUT_DESCENDANTS_N; break;
    }
    /* No `default:` arm, so clang's -Wswitch (on by default) warns about a kind with no case here; such a
       kind, or an integer cast into the enum, leaves `n` at zero and fails the check below. */
    DCHECKF(n >= 1,
            "the rest-point granularity for kind %d is %zu — a unit of zero items is a step that performs no "
            "work and a driver that never ends, and a unit is at least one BY ARCHITECTURE: what this policy "
            "may tune is how often a span offers to rest, never whether it can rest at all. Either this kind has no entry in rest_unit.c's switch or something cast an integer that "
            "is not a RestUnitKind",
            (int)kind, n);
    return n;
}

/* Bound (2), measured. Everything below is dev-only and observes: it drops, truncates and reorders nothing. */

#if APICLIENT_DEV
/* The open bracket: a flag plus the kind, since every enum value is a real kind with a number. One copy
   per process is correct because an engine instance's flows all run on its own thread and this reading is
   that thread's (as solver/quantum.c's slice state is); run-test262, the one multi-threaded host, links no
   engine/host file. A host driving two engines on two threads needs this per thread, and the bracket's
   pairing DCHECKs would fire first. */
static int          g_rest_open;
static RestUnitKind g_rest_kind;
static int64_t      g_rest_mark_us;
#endif

void rest_unit_begin(RestUnitKind kind)
{
#if APICLIENT_DEV
    DCHECK(!g_rest_open,
           "a rest unit was opened inside another one — the bracket measures ONE unit's work against the "
           "scheduler's slice, so a nested pair measures a span that is not a unit and the inner end would "
           "close the outer reading. A driver holds one unit at a time by construction: it performs the unit "
           "and returns to the frontier");
    g_rest_open    = 1;
    g_rest_kind    = kind;
    g_rest_mark_us = quantum_thread_us();
#else
    (void)kind;
#endif
}

void rest_unit_end(void)
{
#if APICLIENT_DEV
    int64_t spent;

    DCHECK(g_rest_open,
           "a rest unit was closed with none open — rest_unit_begin and rest_unit_end are a pair around the "
           "work of exactly one unit, and an unmatched end is a driver that returned to the frontier from "
           "somewhere its own bracket does not cover");
    g_rest_open = 0;
    spent = quantum_thread_us() - g_rest_mark_us;
    /* Two gates. The reading must be CPU (solver/quantum.h): on the wasm host it is a wall clock, which a
       descheduled thread would fail while consuming nothing. And the unit must be a choice (more than one
       item): at one item there is no smaller number to name, so an overrun is fixed one layer down. */
    if (!quantum_measure_is_cpu() || rest_unit_items(g_rest_kind) <= 1)
        return;
    DCHECKF(spent < (int64_t)ENGINE_QUANTUM_MS * 1000,
            "one rest unit of kind %d (%zu items) consumed %lld us of CPU — a whole cooperative quantum is "
            "%d ms, and the entire argument for resting at a chosen granularity rather than at the substrate's "
            "finest item is that a coarser unit DELAYS a due suspension by at most one unit's work, which is "
            "acceptable only while that work is small against the slice the scheduler already lets a flow hold. "
            "At this size it is not: a higher-value sibling that became runnable at the start of this unit "
            "waited longer than the scheduler's own decision interval to be picked. LOWER THE NUMBER FOR THIS "
            "KIND IN rest_unit.c — it is a policy input for exactly this reason and no loader changes — unless "
            "the per-item cost itself has grown, in which case the substrate's item is what to look at",
            (int)g_rest_kind, rest_unit_items(g_rest_kind), (long long)spent, ENGINE_QUANTUM_MS);
#endif
}
