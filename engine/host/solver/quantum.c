/* The cooperative quantum's asynchronous edge — see solver/quantum.h for what it is and why the measure is
   per host rather than a constant. This file holds nothing but the edge: no policy, no ranking, no knowledge
   of a flow. What to do when the budget is gone lives in preempt_hook, which is the ONE policy §scheduler
   allows, and the whole of what this file does is make the question arrive on time. */
#include "solver/quantum.h"
#include "solver/engine.h"     /* ENGINE_QUANTUM_MS — the slice IS the scheduler's, never a private copy */
#include "check.h"
#include "quickjs.h"           /* JS_RequestFlowYield — the one call the edge makes */

#include <stdio.h>             /* the one line this component says out loud — see quantum_announce */
#include <stdlib.h>            /* the composer's buffer — see quantum_json */
#include <time.h>
#include <string.h>

/* ── WHAT THIS RUN'S NUMBERS ARE DENOMINATED IN, SAID ONCE, BY THE COMPONENT THAT OWNS THE FACT ─────────────
 *
 * quantum.h says what each host CAN measure and quantum_measure() answers it in one string. Nothing said it
 * OUT LOUD: both readers of that string were inside SEAM-ASSERTION MESSAGES, so the fact reached a person only
 * on the runs that ABORTED — never on the runs a person actually compares.
 *
 * AND THE CAVEAT IS NOT ABOUT THE SLICE, IT IS ABOUT THE ORDER, which is the larger half and the one a reader
 * cannot reconstruct from quantum.h alone. On a host with no CPU clock the SLICE is defensible on the wall
 * clock for the reason quantum.h gives — the host that would use the returned thread IS this thread, so wall
 * time in which this thread was descheduled is time nobody was denied. The WFQ'S AGING CHARGE IS NOT: engine.c
 * bills `flow_age_running(now - t0)` in this same currency, and that charge is a COMPARISON BETWEEN FLOWS. A
 * descheduling the OS chose therefore lands on whichever flow happened to be running, moves ITS rank and not
 * its siblings', and changes which flow is picked next — so two runs of ONE artifact over ONE document take
 * different frontier orders, and their census series differ with nothing about the tree differing. That has
 * been measured on this fixture: two runs of one build diverging in live count, fork count and pending bytes
 * from the first few hundred steps onward.
 *
 * THE VARIANCE IS THE HOST AND IS NOT TO BE SILENCED — §scheduler's razor forbids the two things that would
 * remove it (drop the quantum and the engine drives to completion; bound the slice in steps and it is a cap).
 * What was missing is only that the series did not SAY which denomination produced it, so a reader could not
 * tell a frontier difference from an artefact of slicing. This line is that statement and nothing else.
 *
 * AND IT IS THE LARGER SOURCE OF THAT VARIANCE, NEVER THE ONLY ONE — read the paragraph at
 * quantum_measure_is_cpu()'s declaration before concluding that a `true` answer makes a single run a
 * measurement. A CPU clock fixes WHOSE BILL a charge lands on; it cannot fix the ORDER, because the slice is
 * denominated in that same CPU while the work one microsecond of it buys is a property of the machine. Two
 * renderers of this fact asserted otherwise on their `isCpu` arm and had to be corrected.
 *
 * UNCONDITIONAL, NOT DEV-ONLY, because a CONSUMER's contract is checked against it and a consumer may not hold
 * a second answer for a release build: engine/build.mjs THROWS when a run printed the frontier census and not
 * this line. That is §Architecture's field contract — a name read somewhere and written nowhere is a broken
 * contract, and a default is what stops it being a crash — so the writer may not be compiled out from under
 * the reader. The bill is one printf of a constant string, once per instance, not once per slice.
 *
 * AT THE FIRST SLICE, so there is no route to remember: every host that opens a slice announces by
 * construction, and a host that declines this edge (wpt_runner.c drives flows under its own preempt policy)
 * announces nothing and prints no census either — an absence that is the same positive statement on both.
 *
 * AND A LINE WAS ONLY EVER HALF OF IT, WHICH THE RESIDUAL THAT STOOD HERE NAMED AND THIS NOW BUILDS. A line is
 * the output of a host whose output IS lines; the SHIPPED path's output is a DOCUMENT. `result.h` moved the
 * four censuses onto it for that reason, and the denomination belongs there for the same one and with a
 * stronger claim than any of them: it is not a reading of the run at all, it is the property that decides how
 * every census BELOW it may be compared. So quantum_json() is the fact, this printf is one emission of it, and
 * solver/result.c's `_quantum` is the other — one composer, two emission sites, the same bytes, which is the
 * idiom result.h already states for `result_cold_json` and its two neighbours.
 *
 * THE TWO EMISSIONS ARE NOT INTERCHANGEABLE AND EACH SAYS SOMETHING THE OTHER CANNOT. The LINE reports what a
 * STAGE DID: a host that opened no slice prints none, which is why engine/build.mjs can treat `@COLD` with no
 * `@QUANTUM` as a broken contract. The FIELD reports what this HOST IS, so it is on every document a host
 * composes whether or not a slice was ever opened — quantum_json() reads nothing but compile-time constants of
 * this branch. A reader that confused the two would take the absence of a line as the absence of a
 * denomination, which is the one thing that is never true.
 *
 * WHAT REMAINS UNCOVERED IS NAMED WHERE IT IS DECIDED, NOT HERE. The one thing this component can be wrong
 * about is now the STRING, and the DCHECK in quantum_json says so at the composer. */
static int g_announced;
static void quantum_announce(void)
{
    char *j;

    if (g_announced) return;
    g_announced = 1;
    /* ONE MARKER, ONE LINE, AND THE PAYLOAD IS THE COMPOSER'S — build.mjs matches `^@QUANTUM \{.*\}$` and
       JSON.parses it exactly as `lastTwo` does for `@COLD`, `@HEAP` and `@SWAP`, so there is one grammar on
       this seam instead of a bespoke `cpu=… slice=…ms measure=…` that only this marker spoke. That spelling
       also put the free-form measure string LAST so an edit to its prose could not move a parsed field; a JSON
       object has no positions to protect, which is the better answer to the same worry.
       AN OOM HERE IS FATAL RATHER THAN SILENT, and the CHECK is the right macro in both builds: this line is
       what a consumer's contract is checked against, so a run that swallowed the allocation and printed
       nothing would present as the writer having been removed. */
    j = quantum_json();
    CHECK(j != NULL, "quantum: the denomination document could not be allocated at the first slice — the "
                     "announce is what engine/build.mjs checks the frontier census against, so a run that "
                     "silently skipped it would read as this writer having been compiled out");
    printf("@QUANTUM %s\n", j);
    fflush(stdout);
    free(j);
}

/* THE THREE FACTS AS ONE DOCUMENT — quantum.h says why it lives in this file and why it is not a census.
   THE ARITHMETIC, DONE FROM THE FORMAT STRING RATHER THAN ESTIMATED, which is result.c's rule for every
   composer and holds here for the same reason: a truncation does not lose a digit, it loses the closing brace,
   and the document that embeds this one then will not parse. The format's fixed bytes are 34 without its
   conversion specifiers; the widest forms of the two it renders itself are 5 (`false`) and 11 (an int's full
   decimal with sign). 34 + 5 + 11 + 1 = 51, plus the measure string, whose length is ADDED rather than bounded
   — the two literals differ per host branch by hundreds of bytes and a fixed buffer would be a constant chosen
   for whichever branch was edited last.
   `isCpu` IS A JSON BOOLEAN AND NOT A 0/1, WHICH IS A CONTRACT AND NOT A SPELLING. Every row of every census
   on this seam is a number, and the consumers assert exactly that of them in one generic loop; a numeric
   `isCpu` would pass that loop and quietly become a fifth census, when what it states is a yes/no about the
   host. Typed as a boolean it CANNOT be folded in, so extension/bridge.js has to assert it by name — which is
   what a reader of the frontier order actually needs to have been told.
   AND THE ONE THING THAT COULD BE WRONG IS ASSERTED AT ITS ORIGIN. `measure` is interpolated into a JSON
   string, and the two values it can take are string literals in THIS file — so an edit that put a quote, a
   backslash or a control character in one would emit a document that does not parse, and the failure would
   surface as a lost finding set in the trusted zone with nothing pointing back here. Bytes >= 0x80 are not
   checked because they need no escape: JSON is UTF-8 and both literals carry em dashes today. */
char *quantum_json(void)
{
    const char *measure = quantum_measure();
    size_t n = strlen(measure) + 51;
    char *out;
    int m;

#if APICLIENT_DEV
    {
        const unsigned char *p;
        for (p = (const unsigned char *)measure; *p; p++)
            DCHECK(*p != '"' && *p != '\\' && *p >= 0x20,
                   "quantum_measure() returned a string that is not a bare JSON string body — this composer "
                   "interpolates it without escaping because both of its values are literals in this file, so "
                   "a quote, a backslash or a control character added to one emits a result document the "
                   "trusted zone cannot parse and every finding for that page is discarded");
    }
#endif
    out = malloc(n);
    if (!out) return NULL;
    m = snprintf(out, n, "{\"measure\":\"%s\",\"isCpu\":%s,\"sliceMs\":%d}",
                 measure, quantum_measure_is_cpu() ? "true" : "false", (int)ENGINE_QUANTUM_MS);
    DCHECK(m > 0 && (size_t)m < n,
           "the quantum denomination did not fit its buffer — a truncation here does not lose a digit, it "
           "loses the closing brace, so both the `@QUANTUM` line and the result document that embeds this "
           "will not parse. Re-do the byte count above from the format string rather than widening it by eye");
    return out;
}

/* ── EMSCRIPTEN: no CPU clock, no asynchronous edge ────────────────────────────────────────────────────────
   Both are facts about the transport (quantum.h names the requirement that would change them), so what is
   built here is the honest thing this host CAN do: the slice is measured on the only clock it has, and the
   claim that this clock is not a CPU clock is CHECKED rather than assumed. */
#if defined(__EMSCRIPTEN__)

/* THE DAY THE TRANSPORT ARRIVES, THIS BRANCH IS THE WRONG ONE, AND A BUILD IS THE CHEAPEST PLACE TO SAY SO.
   Shared linear memory is the whole of what the asynchronous edge is missing here (quantum.h), so a link that
   HAS it must not quietly keep the polling-only host beside it: the slice would still be evaluated only at
   whichever raise the page's own bytecode happens to reach, and the flag would look like it had bought
   something. */
#if defined(__EMSCRIPTEN_SHARED_MEMORY__)
#error "solver/quantum.c: this wasm link HAS shared memory, so the cooperative quantum can finally have an \
asynchronous edge — build the watchdog thread (it stores the yield request through the address of the main \
thread's own thread-local copy of that byte; see quantum.h) rather than linking this polling-only branch."
#endif

static int64_t g_slice_start_us;
static int  g_slice_open;

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
    return "wall (this host has NO cpu clock and NO asynchronous edge — the engine's realm is an opaque "
           "origin, never crossOriginIsolated, so it cannot hand a watchdog thread the shared memory that "
           "thread would raise the request through; see solver/quantum.h)";
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
    /* THE LATCH LIVES WITH ITS ONLY READER. As a file-scope static it was a `-Wunused-variable` in every
       release link, because the `#if` that compiles the reader out does not compile the object out with it. */
    static int checked;
    struct timespec cpu = { 0, 0 };
    int64_t ta, tc, tb;
    int rc;

    if (checked) return;
    checked = 1;
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
    /* THE CLAIM FIRST, THE CHECK OF IT SECOND. If the check below aborts, the stream already carries what this
       host claimed to be measuring, which is the whole of what the abort is about. */
    quantum_announce();
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

/* THE CLAIM ONE LINE UP, CHECKED — "asked, never assumed", and this is the half that had only ever DECLARED.
   The emscripten branch tests that its CPUTIME clock really is its monotonic clock; this is the same test read
   the other way, and it is the one with CONSUMERS BEHIND IT. `quantum_measure_is_cpu()` is what two DCHECKs
   are GATED ON — solver/rest_unit.c's rest-unit overrun and solver/engine.c's seamless-stretch verdict — and
   §Testing forbids a verdict a loaded machine can falsify. Both of those aborts are legitimate ONLY while this
   thread's clock is thread CPU: were it secretly a wall clock they would fire on a busy box against code that
   consumed nothing, which is exactly the confident false red this project has measured four separate times in
   one session. A `return 1` that nothing tests is that red waiting to happen, and the fact that the platform
   makes it true today is the argument for asserting it rather than the argument against.
   THE TEST. Bracket ONE CLOCK_MONOTONIC read between two of this branch's own readers. If the kernel is
   answering both names from one source, the middle read must land BETWEEN the two outer ones — that is the
   emscripten branch's own reasoning, and here landing inside is the FAILURE rather than the pass. Two distinct
   clocks cannot collide by accident: CLOCK_MONOTONIC counts from boot while this thread's CPU is a subset of
   the time since this THREAD started, so a real pair sits orders of magnitude apart rather than within the
   microsecond or two this bracket is wide.
   ONCE, AT THE FIRST SLICE, because what it tests is a property of the kernel and not of a slice; DEV-ONLY,
   because a release build paying for a diagnostic is a diagnostic that has quietly become behaviour — the same
   two answers the emscripten branch gives for the same two questions. */
static void quantum_check_clock(void)
{
#if APICLIENT_DEV
    /* THE LATCH LIVES WITH ITS ONLY READER — see the same line on the other branch. */
    static int checked;
    struct timespec mono = { 0, 0 };
    int64_t ta, tm, tb;
    int rc;

    if (checked) return;
    checked = 1;
    /* The brackets are taken through this host's OWN reader, so a missing thread-CPU clock is reported by the
       CHECK that already stands at the one place this file reads that clock rather than by a second copy here
       that could disagree with the first. */
    ta = quantum_thread_us();
    rc = clock_gettime(CLOCK_MONOTONIC, &mono);
    tm = (int64_t)mono.tv_sec * 1000000 + mono.tv_nsec / 1000;
    tb = quantum_thread_us();
    /* A FAILED READ IS THE SAME NEWS AS A MISMATCH AND IS NOT SWALLOWED — the emscripten branch records what
       an `if (rc != 0) return;` cost there: a check that quietly declines to run on the very input it exists
       to judge leaves the assumption it guards false with nothing anywhere saying so. */
    DCHECK(rc == 0,
           "this host refused CLOCK_MONOTONIC outright — there is then no second clock to check this thread's "
           "CPU clock against, so quantum_measure_is_cpu()'s 1 is a bare declaration again and the two DCHECKs "
           "gated on it (rest_unit.c's rest-unit overrun, engine.c's seamless-stretch verdict) are verdicts a "
           "loaded machine could falsify");
    DCHECK(!(tm >= ta && tm <= tb),
           "this host answered CLOCK_MONOTONIC from the same source as CLOCK_THREAD_CPUTIME_ID — a reading "
           "taken BETWEEN two of this thread's CPU readings landed between them, which one clock does and two "
           "cannot. quantum_thread_us() is then WALL time while quantum_measure_is_cpu() reports 1, so the "
           "slice, the WFQ's aging charge and every verdict gated on that predicate are denominated in "
           "something this file is telling every reader they are not; measure this platform's real thread CPU "
           "or make this branch answer 0 like the host that has no CPU clock");
#endif
}

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
    /* THE CLAIM FIRST, THE CHECK OF IT SECOND — same order and same reason as the other branch: if the check
       aborts, the stream already carries what this host claimed to be measuring. */
    quantum_announce();
    quantum_check_clock();
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
