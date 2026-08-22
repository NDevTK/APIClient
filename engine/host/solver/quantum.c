/* The cooperative quantum's asynchronous edge — see solver/quantum.h for what it is and why the measure is
   per host rather than a constant. This file holds nothing but the edge: no policy, no ranking, no knowledge
   of a flow. What to do when the budget is gone lives in preempt_hook, which is the ONE policy §scheduler
   allows, and the whole of what this file does is make the question arrive on time. */
#include "solver/quantum.h"
#include "solver/engine.h"     /* ENGINE_QUANTUM_MS — the slice IS the scheduler's, never a private copy */
#include "check.h"
#include "quickjs.h"           /* JS_RequestFlowYield — the one call the edge makes */

#include <time.h>
#include <string.h>

/* ── EMSCRIPTEN: no CPU clock, no asynchronous edge ────────────────────────────────────────────────────────
   Both are facts about the transport (quantum.h names the requirement that would change them), so what is
   built here is the honest thing this host CAN do: the slice is measured on the only clock it has, and the
   claim that this clock is not a CPU clock is CHECKED rather than assumed. */
#if defined(__EMSCRIPTEN__)

static int64_t g_slice_start_us;
static int  g_slice_open;
static int  g_clock_checked;

int64_t quantum_thread_us(void)
{
    struct timespec t;
    /* CLOCK_MONOTONIC, named for what it IS on this host. Asking for CLOCK_THREAD_CPUTIME_ID here would return
       the identical number from the identical source and would read as a CPU measurement to everyone after. */
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0,
          "quantum: the host has no monotonic clock at all — the scheduler cannot bound a slice, so a lone "
          "engine would never return the thread to its host");
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

const char *quantum_measure(void)
{
    return "wall (this host has NO cpu clock and NO asynchronous edge — a single-threaded wasm instance is "
           "interruptible only from shared memory; see solver/quantum.h)";
}

int quantum_measure_is_cpu(void) { return 0; }

/* THE ASSUMPTION, CHECKED. emscripten's clock_time_get answers CLOCK_MONOTONIC and both CPUTIME clocks from one
   emscripten_get_now(), so a CPUTIME reading taken BETWEEN two monotonic readings must land between them — it
   is literally the same function called a third time. A real thread-CPU clock could not: by the first slice
   this instance has fetched, parsed and built a document, so its consumed CPU is far below the wall time since
   the timebase started, and the reading falls outside the bracket.
   THE FAILURE IS GOOD NEWS AND THE MESSAGE SAYS SO: it means this host grew a CPU clock, and the whole
   emscripten branch of this file should then be the native one. It is checked ONCE, at the first slice, because
   what it tests is a property of the toolchain and not of a slice. */
/* AND IT IS DEV-ONLY WORK, so it is compiled out with the asserts that are its only readers — the same question
   preempt_hook's gap census had to answer. Three clock reads once per instance is a small bill, but a release
   build paying ANY of it is a diagnostic that has quietly become behaviour. */
static void quantum_check_clock(void)
{
#if APICLIENT_DEV
    struct timespec cpu = { 0, 0 };
    int64_t ta, tc, tb;
    int rc;

    if (g_clock_checked) return;
    g_clock_checked = 1;
    /* The two brackets are taken through this host's OWN reader, so the missing-monotonic-clock case is
       reported by the CHECK that already stands at the one place this file reads a clock, rather than by a
       second copy of it here that could disagree with the first. */
    ta = quantum_thread_us();
    rc = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    tc = (int64_t)cpu.tv_sec * 1000000 + cpu.tv_nsec / 1000;
    tb = quantum_thread_us();
    /* A FAILED READ IS THE SAME NEWS AS A MISMATCH, and it was being SWALLOWED: this was three
       `if (clock_gettime(...) != 0) return;` lines, one of them carrying a comment saying ENOSYS was "the same
       news, said louder" while the code returned in silence. A check that quietly declines to run on the very
       input it exists to judge is the concealment §Offensive-programming names — the assumption this whole
       branch is built on would be false and nothing anywhere would say so. */
    DCHECK(rc == 0,
           "this host refused CLOCK_THREAD_CPUTIME_ID outright — it is then NOT answering it from "
           "emscripten_get_now() like every other clock, which is the assumption this file's emscripten branch "
           "is built on; find out what it does answer and measure the quantum on that");
    DCHECK(tc >= ta && tc <= tb,
           "this host answered CLOCK_THREAD_CPUTIME_ID from something other than its monotonic clock — it "
           "has a REAL cpu clock now, so the quantum must be measured on it and this file's emscripten "
           "branch replaced by the native one (timer + signal still need the shared-memory transport)");
#endif
}

void quantum_begin(void)
{
    DCHECK(!g_slice_open, "quantum_begin inside an open slice — the budget of the outer slice would be "
                          "silently replaced by the inner one's, and the outer would never expire");
    quantum_check_clock();
    g_slice_start_us = quantum_thread_us();
    g_slice_open = 1;
}

void quantum_end(void)
{
    DCHECK(g_slice_open, "quantum_end with no slice open — the scheduler's bracket is unbalanced, so some step "
                         "ran on a budget that was never opened");
    g_slice_open = 0;
}

int quantum_slice_open(void) { return g_slice_open; }

/* ASKED ONLY INSIDE A SLICE, and that is what catches CODE REACHING THE INTERPRETER WITHOUT THE SCHEDULER'S
   BRACKET. Left unasserted, the two hosts answer OPPOSITE lies and neither says why: on THIS branch the budget
   is measured against a start time of zero, which reads as EXPIRED and would park every flow at its first
   opcode forever; on the native branch nothing is armed, which reads as NEVER expired and lets a flow keep the
   thread with nobody able to ask for it back.
   IT FIRED, AND WHAT IT CAUGHT WAS NOT A FOURTH HOST — it was the two hosts this comment used to certify. It
   said main.c and test_forced.c "reach the interpreter through engine_sched_step", which is true of how they
   run FLOWS and false of how they run BUILTINS: both parse a fetch reply with JS_ParseJSON between two steps
   (main.c's qjs_provide on the trusted zone's reply record), the JSON step machine consulted the flow-control
   preempt policy at each completed value, and
   every reply either host has ever delivered aborted here. The parse now OFFERS the thread to whichever driver
   drove it and consults no policy (quickjs.c's json_parse_step), so this assertion is back to naming the case
   it was written for. wpt_runner.c is the one host that genuinely declines the edge — it drives JS_FlowNew/
   JS_FlowResume under its own preempt policy with no frontier to be fair between — and that decline is
   deliberate rather than a silence, which is the other thing this line exists to keep true. */
int quantum_expired(void)
{
    DCHECK(g_slice_open,
           "the cooperative quantum's budget was asked outside an open slice — whoever is running this flow is "
           "not the scheduler, so the budget being consulted belongs to no slice at all (here: measured from a "
           "start time that was never set, i.e. always expired)");
    return quantum_thread_us() - g_slice_start_us >= (int64_t)ENGINE_QUANTUM_MS * 1000;
}

/* ── NATIVE (Linux): a per-thread CPU timer, delivered to the flow's own thread ─────────────────────────── */
#elif defined(__linux__)

#include <signal.h>
#include <unistd.h>

/* WHICH SIGNAL, and why a real-time one. SIGXCPU is spoken for — §Testing reserves it for the RLIMIT_CPU
   backstop, whose whole point is that it reports through a DIFFERENT signal from the harness's own SIGTERM, and
   a third meaning on that number would collapse two verdicts into one again. SIGALRM/SIGVTALRM are the process
   -wide itimers, which a library may arm at any time. SIGRTMIN is queued, unused by libc, and per-thread by
   delivery here. */
#define QUANTUM_SIGNO (SIGRTMIN)

/* THE ONLY OBJECT THE HANDLER WRITES, and it is `volatile sig_atomic_t` because that is the ONE type C11
   §7.14.1.1 lets a handler touch outside a lock-free atomic. Nothing else in this file is reachable from the
   handler: no malloc, no stdio, no clock, no DCHECK (its message formatting is not async-signal-safe and its
   abort would fire from a context that cannot say where it came from), and above all nothing belonging to the
   runtime, the frontier or a flow — the handler decides NOTHING. It raises a question that the interpreter
   answers, on its own stack, at its next opcode, with the whole policy on that side. */
static volatile sig_atomic_t g_fired;
static timer_t g_timer;
static pid_t   g_timer_tid;
static int     g_timer_made;
static int     g_slice_open;

/* THE ORDER OF THE TWO STORES IS DELIBERATE even though this handler runs to completion before the interrupted
   thread executes another instruction: the flag is what the poll READS, and the request is what makes the poll
   HAPPEN, so raising the request first would be a mechanism that depends on the atomicity for correctness
   rather than merely benefiting from it.
   JS_RequestFlowYield is one relaxed lock-free store to a thread-local byte. It is not on POSIX's
   async-signal-safe list because that list is about the standard library; what makes THIS call safe is what it
   does. Two properties are required, and NEITHER is argued for here — both are DECLARED at the byte itself
   (quickjs.c's g_flow_yield_req), which is the only place that can enforce them: the object is a lock-free
   atomic (a _Static_assert), and it is local-exec TLS (an attribute), so the store is a constant offset from
   the thread pointer with no __tls_get_addr call — the one thing in that store that could have taken the
   loader's lock against the interpreter this handler interrupted. Each is a BUILD failure if it stops holding.
   An earlier version of this paragraph reasoned the second one out from how the tree happens to be compiled;
   that is a property nothing checked, and a handler is the last place to hold one of those. */
static void quantum_on_cpu_edge(int signo)
{
    (void)signo;
    g_fired = 1;
    JS_RequestFlowYield();
}

int64_t quantum_thread_us(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0,
          "quantum: this thread has no CPU clock, so the slice the timer is armed for is unmeasurable and the "
          "WFQ's aging term has nothing to charge in");
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

const char *quantum_measure(void) { return "thread-cpu (timer_create CLOCK_THREAD_CPUTIME_ID, SIGEV_THREAD_ID)"; }

int quantum_measure_is_cpu(void) { return 1; }

static void quantum_make_timer(void)
{
    struct sigaction sa;
    struct sigevent sev;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = quantum_on_cpu_edge;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART, because the edge is not an error and must not surface as one. A slice can be open across a
       host read the scheduler performs on its own behalf, and an EINTR delivered into that read would turn a
       thread-sharing yield into a failed syscall somewhere with no idea what interrupted it. */
    sa.sa_flags = SA_RESTART;
    CHECK(sigaction(QUANTUM_SIGNO, &sa, NULL) == 0,
          "quantum: the CPU-edge signal handler could not be installed — without it the cooperative quantum has "
          "no source at all and a flow in straight-line code never returns the thread to its host");

    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo  = QUANTUM_SIGNO;
    /* SIGEV_THREAD_ID targets ONE thread, and that is the mechanism rather than a nicety: the yield request is
       THREAD-LOCAL (one flow base per thread — run-test262 drives several corpus threads through the same
       runtime), so a signal delivered to any other thread of this process would raise a request in a thread
       that is not running this flow, and the flow it was meant for would never be asked. */
    /* ONE FIELD, TWO SPELLINGS, and the discriminator is the C library rather than a macro test: glibc names it
       `_sigev_un._tid` and defines NO `sigev_notify_thread_id` alias (it aliases only the SIGEV_THREAD members),
       so `#ifdef sigev_notify_thread_id` answers false on BOTH libraries and would pick glibc's spelling on a
       musl build, where the alias is a real struct member and `_sigev_un` does not exist. */
#if defined(__GLIBC__)
    sev._sigev_un._tid = gettid();
#else
    sev.sigev_notify_thread_id = gettid();
#endif
    CHECK(timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &g_timer) == 0,
          "quantum: no per-thread CPU timer could be created — the cooperative quantum would then be measured "
          "on nothing, and §scheduler requires it (a lone engine otherwise freezes in a non-returning step)");
    g_timer_tid = gettid();
    g_timer_made = 1;
}

static void quantum_settime(int64_t ms)
{
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec  = (time_t)(ms / 1000);
    its.it_value.tv_nsec = (long)((ms % 1000) * 1000000L);
    /* it_interval stays zero: ONE SHOT PER SLICE. A periodic timer would re-fire on the host's own time between
       two steps and on the next slice at an offset that has nothing to do with when that slice began — the
       budget is per slice, so the arm is too. */
    CHECK(timer_settime(g_timer, 0, &its, NULL) == 0,
          "quantum: the CPU timer could not be (dis)armed — a slice with no edge never expires and a slice "
          "whose edge outlives it fires on time that belongs to the host");
}

void quantum_begin(void)
{
    DCHECK(!g_slice_open, "quantum_begin inside an open slice — the outer slice's budget would be silently "
                          "replaced by the inner one's and would never expire");
    if (!g_timer_made) quantum_make_timer();
    /* THE TIMER BELONGS TO A THREAD, so the thread it was made for must be the thread about to run the flow.
       If the scheduler ever moved to another thread the signal would keep arriving at the old one: the flow
       would consume the whole CPU it liked and the quantum would silently never fire — a missing seam whose
       only symptom is slowness. Asked at every arm, because a move can happen at any of them. */
    DCHECK(gettid() == g_timer_tid,
           "the quantum's CPU timer is armed for a different thread than the one about to run the flow — the "
           "edge would be delivered to a thread that is not executing the interpreter, so the request the "
           "running flow polls for would never be raised");
    /* ARM FIRST, CLEAR SECOND. A stale fire left pending by the previous slice can only be delivered before
       this returns, so clearing after the arm consumes it; clearing before would let it survive into this
       slice and expire the budget on the first opcode. The new arm cannot fire in the gap — it is a whole
       quantum of CPU away. */
    quantum_settime(ENGINE_QUANTUM_MS);
    g_fired = 0;
    g_slice_open = 1;
}

void quantum_end(void)
{
    DCHECK(g_slice_open, "quantum_end with no slice open — the scheduler's bracket is unbalanced, so some step "
                         "ran on a budget that was never opened");
    quantum_settime(0);
    g_fired = 0;
    g_slice_open = 0;
}

int quantum_slice_open(void) { return g_slice_open; }

/* ASKED ONLY INSIDE A SLICE — see the same assertion on the other branch for what it caught and why the claim
   "both hosts reach the interpreter through engine_sched_step" was true of their FLOWS and false of their
   BUILTINS. Here the silent answer is the opposite one and no less wrong: no timer is armed outside a slice,
   so the budget reads NEVER expired and code driven outside the bracket holds the thread with nothing able to
   ask for it back. test_forced.c's fixture_provide is the native shape of the same defect — it answers a park
   from run_scheduler's stall seam, between two slices, and engine_provide learns from the reply's body there. */
int quantum_expired(void)
{
    DCHECK(g_slice_open,
           "the cooperative quantum's budget was asked outside an open slice — whoever is running this flow is "
           "not the scheduler, so no CPU timer is armed for it and the budget can never expire; the flow holds "
           "the thread and the host is never given a turn to ask for it back");
    return g_fired != 0;
}

#else
#error "solver/quantum.c: this host has no cooperative-quantum edge. §scheduler requires one (a lone engine \
otherwise freezes in a non-returning step), so add the branch that measures this platform's CPU and raises \
JS_RequestFlowYield from outside the flow's own instruction stream — never a silent fall-through to a clock."
#endif
