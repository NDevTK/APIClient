/* quickjs-unpriced.h — the spans in which a dev build computes what a release build does not.
 *
 * Engine work is a function of the flow's path, the same in a dev and a release build of one revision. A dev
 * build also evaluates every assert's condition and runs the second derivations that check a first one, so
 * each of those runs inside a span, and while a span is open on the thread nothing it does is work: a host
 * charge retires nothing (quickjs.h's JS_FlowRetireWork, core/timing/thread_work.h), an allowance metering the
 * same input is not spent, and the interpreter, the compiler, the matcher and the scheduler's rest question
 * assert that no span is open. A release build has no span.
 *
 * It lives beside quickjs-check.h because both assert headers open a span around a DCHECK's condition and the
 * fork's translation units build without the host's include roots. The depth is the thread's own, as the work
 * counter is (run-test262 runs one runtime per thread), and a weak definition, as quickjs-gap.h's log is. */
#ifndef QUICKJS_UNPRICED_H
#define QUICKJS_UNPRICED_H

#if defined(APICLIENT_DEV) && APICLIENT_DEV == 0
#define APICLIENT_UNPRICED_OPEN() 0
#else
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) _Thread_local int apiclient_unpriced_depth;
#else
#error "quickjs-unpriced.h needs a weak definition; this compiler states none"
#endif

/* Whether a span is open on this thread. An assert's condition is itself a span, so an assert that no span is
   open is written DCHECK_PRICED, which reads this before it opens one. */
#define APICLIENT_UNPRICED_OPEN() (apiclient_unpriced_depth != 0)

/* Closes the span APICLIENT_UNPRICED opened and hands its operand's truth through. */
static inline int apiclient_unpriced_close(int held)
{
    apiclient_unpriced_depth--;
    return held;
}

/* The truth of `expr`, evaluated inside a span. Spans nest: an assert in a callee of an assert's condition
   opens and closes its own. */
#define APICLIENT_UNPRICED(expr) (apiclient_unpriced_depth++, apiclient_unpriced_close(!!(expr)))

/* A compound statement run inside a span, `APICLIENT_UNPRICED_BLOCK { … }`: the reads and the second
   derivation a `#if APICLIENT_DEV` region makes to check a first. It exists only in a dev build. The block
   leaves by falling off its end; a `break`, `return` or `goto` out of it leaves the span open, which the
   scheduler's next step asserts.
   Named residual. Not covered: a `#if APICLIENT_DEV` region whose statements call priced code outside this
   block is still charged in a dev build only. Next diff builds: engine/dcheckgate.mjs refusing a call in such
   a region outside an assert and outside this block. Absence shows as: engine/regimeparity.mjs's work rows
   differing between the two regimes of one revision. */
#define APICLIENT_UNPRICED_BLOCK \
    for (int apiclient_unpriced_once_ = (apiclient_unpriced_depth++, 1); apiclient_unpriced_once_; \
         apiclient_unpriced_once_ = (apiclient_unpriced_depth--, 0))
#endif

#endif /* QUICKJS_UNPRICED_H */
