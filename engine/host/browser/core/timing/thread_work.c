/* Work the thread does outside the interpreter's dispatch. See thread_work.h. */
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/timing/event_loop.h"
#include "core/timing/thread_work.h"
#include "solver/quantum.h"

/* Lifetime counts over the process, never reset, like the clock asks they are read beside. */
static long     g_spans[THREAD_WORK_N];
static uint64_t g_bytes[THREAD_WORK_N];
static int64_t  g_cpu_us[THREAD_WORK_N];
static uint64_t g_discarded;
static uint64_t g_charged;
static uint64_t g_charged_input[THREAD_WORK_N];
/* The open spans' inner time: `g_inner[d]` is the thread CPU the spans nested directly inside the open span at
   depth `d` took, which that span subtracts from its own. */
static int64_t *g_inner;
static int      g_depth, g_inner_cap;
#if APICLIENT_DEV
/* `g_inner_input[d]`: the input-byte kinds (thread_work_kind_is_input) whose spans ended inside the open span at
   depth `d`, at any nesting. A span of one of those kinds that ends over one of its own kind counts the same
   bytes twice, once per seam. */
static uint32_t *g_inner_input;
_Static_assert(THREAD_WORK_N <= 32, "the input-kind mask is one bit per kind");

static bool thread_work_kind_is_input(ThreadWorkKind kind)
{
    return kind == THREAD_WORK_DECODE || kind == THREAD_WORK_HTML_PARSE || kind == THREAD_WORK_CSS_PARSE ||
           kind == THREAD_WORK_XML_PARSE;
}
#endif

void thread_work_begin(ThreadWorkSpan *s)
{
    DCHECK(g_depth >= 0, "a thread-work span ended that never began");
    if (g_depth == g_inner_cap) {
        int cap = g_inner_cap ? 2 * g_inner_cap : 64;
        int64_t *grown = realloc(g_inner, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "thread_work: OOM growing the span stack");
        g_inner = grown;
#if APICLIENT_DEV
        {
            uint32_t *kinds = realloc(g_inner_input, (size_t)cap * sizeof *kinds);

            CHECK(kinds != NULL, "thread_work: OOM growing the span stack");
            g_inner_input = kinds;
        }
#endif
        g_inner_cap = cap;
    }
    s->depth = g_depth;
#if APICLIENT_DEV
    g_inner_input[g_depth] = 0;
#endif
    g_inner[g_depth++] = 0;
    s->t0 = quantum_thread_us();
}

void thread_work_end(ThreadWorkSpan *s, ThreadWorkKind kind, uint64_t bytes)
{
    int64_t took;

    DCHECK(kind < THREAD_WORK_N, "a thread-work span ended under a kind this census has no row for");
    DCHECK(g_depth > 0, "a thread-work span ended with none open");
    DCHECKF(s->depth == g_depth - 1, "a thread-work span begun at depth %d ended at depth %d, so an inner span is "
            "still open", s->depth, g_depth - 1);
    took = quantum_thread_us() - s->t0;
    g_depth--;
    if (g_depth > 0) g_inner[g_depth - 1] += took;
#if APICLIENT_DEV
    if (thread_work_kind_is_input(kind)) {
        DCHECKF(bytes == 0 || !(g_inner_input[g_depth] & (1u << kind)),
                "a thread-work span of input kind %d ended over an inner span of the same kind, so its bytes are "
                "counted and charged once per seam: charge input at the innermost seam that parses it and drop "
                "the outer span", (int)kind);
        if (g_depth > 0 && bytes > 0) g_inner_input[g_depth - 1] |= 1u << kind;
    }
    if (g_depth > 0) g_inner_input[g_depth - 1] |= g_inner_input[g_depth];
#endif
    g_spans[kind]++;
    g_bytes[kind] += bytes;
    g_cpu_us[kind] += took - g_inner[g_depth];
}

void thread_work_charge(ThreadWorkKind kind, uint64_t bytes)
{
    static const uint64_t PER_KIB[THREAD_WORK_N] = {
        [THREAD_WORK_DECODE] = THREAD_WORK_UNITS_PER_KIB_DECODE,
        [THREAD_WORK_HTML_PARSE] = THREAD_WORK_UNITS_PER_KIB_HTML,
        [THREAD_WORK_CSS_PARSE] = THREAD_WORK_UNITS_PER_KIB_CSS,
        [THREAD_WORK_XML_PARSE] = THREAD_WORK_UNITS_PER_KIB_XML,
        [THREAD_WORK_CONSTRAINT] = THREAD_WORK_UNITS_PER_KIB_CONSTRAINT,
        [THREAD_WORK_ORPHAN_WALK] = THREAD_WORK_UNITS_PER_KI_ORPHAN_ENTRIES,
        [THREAD_WORK_INTL] = THREAD_WORK_UNITS_PER_KIB_INTL,
        [THREAD_WORK_LAYOUT_BOX] = THREAD_WORK_UNITS_PER_KI_LAYOUT_BOXES,
        [THREAD_WORK_LAYOUT_LINE] = THREAD_WORK_UNITS_PER_KI_LINE_ITEMS,
        [THREAD_WORK_SELECTOR_MATCH] = THREAD_WORK_UNITS_PER_KI_SELECTOR_TESTS,
    };
    uint64_t units;

    DCHECK(kind != THREAD_WORK_SCRIPT_PARSE && kind < THREAD_WORK_N,
           "a charge named script parse or no kind — quickjs's tokenizer charges script parse itself");
    /* Rounded up per call: a step over a non-empty input costs at least the one unit an opcode does, and a
       remainder carried between calls would make one flow's charge depend on the flow that ran before it. Rounding
       down would make every constraint probe under 171 bytes free, and a lookup is one probe per frozen segment. */
    units = (bytes * PER_KIB[kind] + 1023) / 1024;
    g_charged += units;
    g_charged_input[kind] += bytes;
    if (units != 0) JS_FlowRetireWork(units);
}

void thread_work_note_discarded(uint64_t units) { g_discarded += units; }

void thread_work_census(ThreadWorkCensus *out)
{
    int k;

    DCHECK(out != NULL, "the thread-work census was asked into nowhere");
    out->banked_units = event_loop_work_banked_life();
    out->charged_units = g_charged;
    out->discarded_units = g_discarded;
    out->script_parse_bytes_all = JS_FlowParseBytesLife();
    for (k = 0; k < THREAD_WORK_N; k++) {
        out->spans[k] = g_spans[k];
        out->bytes[k] = g_bytes[k];
        out->cpu_us[k] = g_cpu_us[k];
        out->charged_input[k] = g_charged_input[k];
    }
}
