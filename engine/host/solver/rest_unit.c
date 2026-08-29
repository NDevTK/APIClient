/* The rest-point granularity policy — see solver/rest_unit.h for what a rest unit is, why the scheduler owns
   it rather than the component that rests, and the two bounds every answer below has to satisfy.

   THIS FILE HOLDS THE NUMBERS AND THE REASON FOR EACH, AND NOTHING ELSE. It has no loop, no queue, no handle
   and no knowledge of a parser: a component asks how many of its own items one rest point covers and does its
   own counting. That is what makes "two components cannot disagree about the unit" true by construction rather
   than by review — core/loader/html_document.c and core/loader/text_document.c fill the same input byte stream
   under the same sentence of the standard, and there is exactly one expression of that unit in the tree for
   them to read. */
#include "solver/rest_unit.h"

#include "solver/engine.h"     /* ENGINE_QUANTUM_MS — the ceiling is the SCHEDULER's slice, never a private copy */
#include "solver/quantum.h"    /* the consumed-CPU reading, and whether this host's reading IS cpu */
#include "check.h"

/* HTML §7.5.2 "Loading HTML documents" / §7.5.4 "Loading text documents": ONE NETWORKING TASK'S WORTH OF THE
   FETCHED BYTES, as this engine sizes it. The standard names the unit and not its size, and it cannot: a task's
   bytes are however many the transport delivered, and this engine's transport delivers none of them — the
   trusted zone hands the loader a whole decoded entity, so there is no observed arrival to take the size from
   and the policy must state one.

   WHAT DECIDES IT is the pair of bounds rest_unit.h states, and nothing else:
     - the FLOOR is the cost of asking. One rest point is a step-machine yield, a frontier re-rank and possibly
       a COW delta swap, and at one byte per rest that cost is paid once per byte of every document the engine
       loads, dwarfing the tokenizer work it brackets. 16 KiB pays it once per sixteen thousand bytes, which
       makes it invisible beside the parse — and nothing beyond that point is worth buying, because the cost is
       already gone.
     - the CEILING is the scheduler's own slice. A coarser unit delays a due suspension by at most one unit, so
       one unit's parse must stay small against ENGINE_QUANTUM_MS; below that interval the delay cannot change
       which flow runs next, because the scheduler does not make a decision that finely. 16 KiB of tokenizer
       and §13.2.6 tree construction is far under one 12 ms slice of CPU on any throughput this engine has ever
       shown — and it is ASSERTED rather than left as a claim, by the bracket at the bottom of this file.
   Between those two the exact value is free, which is the point of it being a policy input: it moves here,
   with the reasoning, and no loader changes. */
#define REST_UNIT_INPUT_BYTES_N   16384u

/* HTML §7.5.3 "Loading XML documents": ONE CONSTRUCT, and it stays one.
   A multiplier here would multiply a quantity the DOCUMENT chooses. One construct is one instance of a
   production of XML §2.1 "Well-Formed XML Documents"' [1] `document`, and [14] `CharData` is a character run as
   long as the response cares to make it — so "four constructs" is not four times a fixed cost, it is four
   times an unbounded one, which is bound (1) of rest_unit.h broken by a number that looks like a granularity.
   The floor argument does not apply either: a construct is already coarse enough that the rest cost is
   amortized, which is exactly the difference between this unit and the per-byte one it is being contrasted
   with. And a document whose ONE construct overruns the slice is not a policy error — it is core/xml/'s
   [14] scan needing to become a pull itself, one layer down, which is a change to that walk and not to this
   number. */
#define REST_UNIT_XML_CONSTRUCTS_N   1u

/* §7.5.3's discard of the partial tree a failed parse left: ONE TOP-LEVEL CHILD, for the construct's reason
   with the unboundedness one step more obvious — removing a child removes its whole subtree, and the document
   chose that subtree. */
#define REST_UNIT_SUBTREE_REMOVALS_N   1u

/* HTML §14.2 "Parsing XML documents"' script question, asked of each node of a finished tree: 1024 NODES.
   This is the one XML unit whose item is FIXED work — a node-type test, a namespace and local-name extraction
   into fixed-size buffers, and two comparisons against short literals — so a multiplier multiplies a constant
   and bound (1) is untouched. It is the per-byte defect in miniature otherwise: a pure inspection walk over a
   response-sized tree, one scheduler round trip per predicate. 1024 predicates is a few microseconds, three
   orders of magnitude under the slice. */
#define REST_UNIT_NODE_VISITS_N   1024u

size_t rest_unit_items(RestUnitKind kind)
{
    size_t n = 0;

    switch (kind) {
    case REST_UNIT_INPUT_BYTES:      n = REST_UNIT_INPUT_BYTES_N;      break;
    case REST_UNIT_XML_CONSTRUCTS:   n = REST_UNIT_XML_CONSTRUCTS_N;   break;
    case REST_UNIT_SUBTREE_REMOVALS: n = REST_UNIT_SUBTREE_REMOVALS_N; break;
    case REST_UNIT_NODE_VISITS:      n = REST_UNIT_NODE_VISITS_N;      break;
    }
    /* NOT A `default:` ARM — the switch is exhaustive over RestUnitKind, so a kind added to the header with no
       number here does not compile rather than silently reaching the crash below. What lands here is a value
       that is in NEITHER list, which is a caller that cast an integer into the enum. */
    DCHECKF(n >= 1,
            "the rest-point granularity for kind %d is %zu — a unit of zero items is a step that performs no "
            "work and a driver that never ends, and a unit is at least one BY ARCHITECTURE: what this policy "
            "may tune is how often a span offers to rest, never whether it can rest at all (CLAUDE.md §NO "
            "BOUNDS). Either this kind has no entry in rest_unit.c's switch or something cast an integer that "
            "is not a RestUnitKind",
            (int)kind, n);
    return n;
}

/* ── THE CEILING, MEASURED ──────────────────────────────────────────────────────────────────────────────────
   Everything below is dev-only and observes: it truncates nothing, drops no flow and changes no schedule. */

#if APICLIENT_DEV
/* WHICH BRACKET IS OPEN, as the kind itself plus a flag — a flag rather than a sentinel kind, because every
   value of the enum is a real kind and inventing a NONE member to mean "no bracket" would put a value in the
   policy switch that has no number and no cost sentence.
   ONE COPY PER PROCESS AND NOT PER THREAD, which is a claim worth stating because the shape has been wrong
   here before: a process-wide counter asserted about by several corpus THREADS fired on a race between two
   healthy ones. This one is safe for the reason solver/quantum.c's slice state is — an engine instance's flows
   all run on the instance's own thread, this reading is that thread's, and the only host that drives corpus
   work on several threads (run-test262) links no file under engine/host at all. A host that ever drove two
   engines on two threads would need this per thread, and the unbalanced-bracket DCHECKs below are what would
   say so first. */
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
    /* THE CHECK IS GATED TWICE, AND BOTH GATES ARE THE POINT.
       (a) `quantum_measure_is_cpu` — §Testing forbids a verdict a loaded machine can falsify, and on the wasm
           host this reading is emscripten's wall clock (solver/quantum.h says so in one place so no message
           has to restate it). A descheduled thread would fail this on a busy box while consuming nothing.
       (b) `rest_unit_items(kind) > 1` — the unit is only a POLICY choice where the answer is more than the
           substrate's own item. At 1 there is no smaller number to name, so an overrun is a fact about the
           document and the fix is one layer down, in the walk that produced the item. Asserting there would
           be a red naming a change nobody can make. */
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
