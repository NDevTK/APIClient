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

/* The host's turn: wall microseconds a slice may hold the thread before the host takes its turn inside a step,
   0 for never, and the wall reading the current hold began at (the slice's open or the last turn's end). */
static int64_t g_turn_us;
static int64_t g_turn_open_us;

/* CLOCK_MONOTONIC on every host: the turn is about elapsed time, whichever clock prices work. */
static int64_t quantum_wall_us(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0,
          "quantum: the host has no monotonic clock, so a host turn cannot be answered on time");
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* The slice's budget in engine work. */
static int64_t g_slice_work = ENGINE_QUANTUM_WORK;

void quantum_set_slice_work(int64_t work)
{
    DCHECKF(work > 0, "a slice of %lld units was set — a slice hands the thread over after positive work",
            (long long)work);
    DCHECK(!g_slice_open, "a slice's budget was set inside a slice — it is the driver's policy, stated between slices");
    g_slice_work = work;
}

int64_t quantum_slice_work(void) { return g_slice_work; }

void quantum_set_host_turn_us(int64_t us)
{
    DCHECKF(us >= 0, "a host turn of %lld us was set — 0 is never, and a turn is a duration", (long long)us);
    DCHECK(!g_slice_open, "a host turn was set inside a slice — it is the host's policy, stated between steps");
    g_turn_us = us;
}

/* The host's entry for a turn inside a step, the work offered since the clock was last read, whether the entry
   is running now, and the turns taken per site. */
static void (*g_step_turn)(void);
static uint64_t g_turn_offered;
static int g_turn_taking;
static long long g_step_turns[QUANTUM_TURN_N];

void quantum_set_step_turn(void (*take)(void))
{
    DCHECK(take != NULL && g_turn_us > 0,
           "a host stated an in-step turn with no entry or no turn duration — the entry is taken on elapsed time");
    DCHECK(!g_slice_open, "an in-step turn was stated inside a slice — it is the host's policy, stated between steps");
    g_step_turn = take;
}

void quantum_step_turn_point(QuantumTurnSite site, uint64_t units)
{
    EngineTurnWitness w0, w1;

    DCHECKF(site < QUANTUM_TURN_N, "a turn was offered at site %d, which this component does not count", (int)site);
    DCHECK(!g_turn_taking, "a turn was offered while the host's own turn runs — the host entered the engine while a "
                           "step waited on its stack");
    if (g_step_turn == NULL || !g_slice_open) return;
    g_turn_offered += units;
    if (g_turn_offered < (uint64_t)ENGINE_QUANTUM_ASK_EVERY) return;
    g_turn_offered = 0;
    if (quantum_wall_us() - g_turn_open_us < g_turn_us) return;
    engine_turn_witness(&w0);
    g_turn_taking = 1;
    g_step_turn();
    g_turn_taking = 0;
    engine_turn_witness(&w1);
    DCHECKF(w0.service_work == w1.service_work && w0.residency_work == w1.residency_work &&
                w0.standing == w1.standing && w0.frontier_gen == w1.frontier_gen && w0.running == w1.running,
            "the host's turn inside a step moved the engine (service %lld -> %lld, residency %lld -> %lld, standing "
            "%llu -> %llu, generation %u -> %u, running flow %s) — the host made an entry while the step waited, "
            "and the step would resume on a state its path did not reach",
            (long long)w0.service_work, (long long)w1.service_work, (long long)w0.residency_work,
            (long long)w1.residency_work, (unsigned long long)w0.standing, (unsigned long long)w1.standing,
            w0.frontier_gen, w1.frontier_gen, w0.running == w1.running ? "kept" : "changed");
    g_step_turns[site]++;
    g_turn_open_us = quantum_wall_us();
}

long long quantum_step_turns(QuantumTurnSite site)
{
    DCHECKF(site < QUANTUM_TURN_N, "the turns at site %d were asked, which this component does not count", (int)site);
    return g_step_turns[site];
}

/* ── Emscripten: the pricing clock is the wall clock ─────────────────────────────────────────────────────── */
#if defined(__EMSCRIPTEN__)

/* CLOCK_MONOTONIC, named for what it is: emscripten answers every WASI clock, the CPU-time ones included, from
   emscripten_get_now(), so asking for CLOCK_THREAD_CPUTIME_ID would return this same number under a CPU name. */
int64_t quantum_thread_us(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0,
          "quantum: the host has no monotonic clock, so the census has no clock to price work on");
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

const char *quantum_measure(void) { return "wall (emscripten answers every WASI clock from emscripten_get_now)"; }
int quantum_measure_is_cpu(void) { return 0; }

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

static void quantum_slice_mark(void) { quantum_check_clock(); }

/* ── Native: the pricing clock is this thread's CPU ─────────────────────────────────────────────────────── */
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

static void quantum_slice_mark(void) {}

#else
#error "solver/quantum.c: name this host's pricing clock"
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
    return composef("{\"sliceWork\":%lld,\"price\":\"%s\",\"priceIsCpu\":%s}",
                    (long long)g_slice_work, measure, quantum_measure_is_cpu() ? "true" : "false");
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
    if (g_turn_us > 0) g_turn_open_us = quantum_wall_us();
    g_slice_open = 1;
}

void quantum_end(void)
{
    DCHECK(g_slice_open, "quantum_end with no slice open — the scheduler's bracket is unbalanced");
    g_slice_open = 0;
}
