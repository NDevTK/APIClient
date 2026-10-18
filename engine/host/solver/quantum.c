/* The cooperative slice's bracket, the host's claim on the thread, and the pricing clock (solver/quantum.h). */
#include "solver/quantum.h"
#include "solver/engine.h"     /* ENGINE_QUANTUM_WORK: the slice the document states is the scheduler's */
#include "solver/compose.h"    /* this document is sized by what it writes */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_slice_open;

int quantum_slice_open(void) { return g_slice_open; }

/* ── Emscripten: the renderer frame's event loop runs only between two qjs_step calls ─────────────────── */
#if defined(__EMSCRIPTEN__)

/* Wall milliseconds a slice may hold the renderer frame's thread before the host gets it back: a
   responsiveness policy for the frame's own event loop, not a share of anything. */
#define QUANTUM_HOST_YIELD_MS 12

static int64_t g_slice_open_us;

/* CLOCK_MONOTONIC, named for what it is: emscripten answers every WASI clock, the CPU-time ones included, from
   emscripten_get_now(), so asking for CLOCK_THREAD_CPUTIME_ID would return this same number under a CPU name. */
int64_t quantum_thread_us(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0,
          "quantum: the host has no monotonic clock, so the renderer frame cannot be handed back on time");
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

const char *quantum_measure(void) { return "wall (emscripten answers every WASI clock from emscripten_get_now)"; }
int quantum_measure_is_cpu(void) { return 0; }
static int quantum_host_yield_ms(void) { return QUANTUM_HOST_YIELD_MS; }

/* The label above, checked once per instance: a CPU-time reading taken between two monotonic readings lands
   between them only if both come from one clock. A toolchain with a real thread CPU clock fails this, and the
   pricing clock should then read it and say "thread-cpu". */
static void quantum_check_clock(void)
{
#if APICLIENT_DEV
    static int checked;
    struct timespec cpu = { 0, 0 };
    int64_t ta, tc, tb;
    int rc;

    if (checked) return;
    checked = 1;
    ta = quantum_thread_us();
    rc = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    tc = (int64_t)cpu.tv_sec * 1000000 + cpu.tv_nsec / 1000;
    tb = quantum_thread_us();
    DCHECKF(rc == 0 && tc >= ta && tc <= tb,
            "this host answered CLOCK_THREAD_CPUTIME_ID (rc %d, %lld) from something other than its monotonic "
            "clock (%lld..%lld) — it has a thread CPU clock, so the pricing clock should read it and "
            "quantum_measure should say so", rc, (long long)tc, (long long)ta, (long long)tb);
#endif
}

static void quantum_slice_mark(void) { quantum_check_clock(); g_slice_open_us = quantum_thread_us(); }

int quantum_host_wants_thread(void)
{
    DCHECK(g_slice_open, "the host's claim on the thread was asked outside an open slice — whoever is running "
                         "this flow is not the scheduler");
    return quantum_thread_us() - g_slice_open_us >= (int64_t)QUANTUM_HOST_YIELD_MS * 1000;
}

/* ── Native: one engine per process, so the OS shares the machine and no host waits on a step ───────────── */
#elif defined(__linux__) || defined(__APPLE__)

int64_t quantum_thread_us(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0,
          "quantum: this thread has no CPU clock, so the censuses that price work have nothing to read");
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

const char *quantum_measure(void) { return "thread-cpu (CLOCK_THREAD_CPUTIME_ID)"; }
int quantum_measure_is_cpu(void) { return 1; }
static int quantum_host_yield_ms(void) { return 0; }

static void quantum_slice_mark(void) {}

int quantum_host_wants_thread(void)
{
    DCHECK(g_slice_open, "the host's claim on the thread was asked outside an open slice — whoever is running "
                         "this flow is not the scheduler");
    return 0;
}

#else
#error "solver/quantum.c: name this host's pricing clock and say whether a host event loop waits on a step"
#endif

/* Interpolated into JSON unescaped, so the string must carry no quote, backslash or control byte; both values
   are literals of this file and the dev build checks whichever one this host compiled. */
char *quantum_json(void)
{
    const char *measure = quantum_measure();

#if APICLIENT_DEV
    for (const unsigned char *p = (const unsigned char *)measure; *p; p++)
        DCHECK(*p != '"' && *p != '\\' && *p >= 0x20,
               "quantum_measure() returned a string that is not a bare JSON string body");
#endif
    return composef("{\"sliceWork\":%lld,\"hostYieldMs\":%d,\"price\":\"%s\",\"priceIsCpu\":%s}",
                    (long long)ENGINE_QUANTUM_WORK, quantum_host_yield_ms(), measure,
                    quantum_measure_is_cpu() ? "true" : "false");
}

/* Printed once per instance, at its first slice: the line reports that a stage opened a slice, the `_quantum`
   field what this host is. An allocation failure is fatal, since a missing line reads as a removed writer. */
static void quantum_announce(void)
{
    static int announced;
    char *j;

    if (announced) return;
    announced = 1;
    j = quantum_json();
    CHECK(j != NULL, "quantum: the slice document could not be allocated at the first slice");
    printf("@QUANTUM %s\n", j);
    fflush(stdout);
    free(j);
}

void quantum_begin(void)
{
    DCHECK(!g_slice_open, "quantum_begin inside an open slice — the scheduler's bracket is unbalanced");
    quantum_announce();
    quantum_slice_mark();
    g_slice_open = 1;
}

void quantum_end(void)
{
    DCHECK(g_slice_open, "quantum_end with no slice open — the scheduler's bracket is unbalanced");
    g_slice_open = 0;
}
