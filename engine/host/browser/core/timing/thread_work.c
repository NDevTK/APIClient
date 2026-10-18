/* Work the thread does outside the interpreter's dispatch. See thread_work.h. */
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
static int      g_depth;

void thread_work_begin(ThreadWorkSpan *s)
{
    DCHECK(g_depth >= 0, "a thread-work span ended that never began");
    s->outer = (g_depth++ == 0);
    s->t0 = s->outer ? quantum_thread_us() : 0;
}

void thread_work_end(ThreadWorkSpan *s, ThreadWorkKind kind, uint64_t bytes)
{
    DCHECK(kind < THREAD_WORK_N, "a thread-work span ended under a kind this census has no row for");
    DCHECK(g_depth > 0, "a thread-work span ended with none open");
    g_depth--;
    if (!s->outer) return;
    DCHECK(g_depth == 0, "the outermost thread-work span ended while an inner one was still open");
    g_spans[kind]++;
    g_bytes[kind] += bytes;
    g_cpu_us[kind] += quantum_thread_us() - s->t0;
}

void thread_work_charge(ThreadWorkKind kind, uint64_t bytes)
{
    static const uint64_t PER_KIB[THREAD_WORK_N] = {
        [THREAD_WORK_DECODE] = THREAD_WORK_UNITS_PER_KIB_DECODE,
        [THREAD_WORK_HTML_PARSE] = THREAD_WORK_UNITS_PER_KIB_HTML,
        [THREAD_WORK_CSS_PARSE] = THREAD_WORK_UNITS_PER_KIB_CSS,
        [THREAD_WORK_CONSTRAINT] = THREAD_WORK_UNITS_PER_KIB_CONSTRAINT,
    };
    uint64_t units;

    DCHECK(kind != THREAD_WORK_SCRIPT_PARSE && kind < THREAD_WORK_N,
           "a charge named script parse or no kind — quickjs's tokenizer charges script parse itself");
    units = bytes * PER_KIB[kind] / 1024;
    g_charged += units;
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
    }
}
