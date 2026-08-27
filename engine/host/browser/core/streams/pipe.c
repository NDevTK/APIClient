/* PIPING — the Streams Standard §4.2.4's ReadableStreamPipeTo, and §4.2's `pipeTo` and `pipeThrough` over it.
 *
 * WHY IT IS ITS OWN COMPONENT. The algorithm is not §4's and it is not §5's: it holds a reader on one and a
 * writer on the other, and every one of its decisions reads state from both. Written inside either half it
 * would have to reach into the other's records; written here it reaches BOTH through their declared abstract
 * operations — the same ones a page performs, taken as the ORIGINAL function objects, so a page that rebinds
 * `ReadableStreamDefaultReader.prototype.read` changes what IT calls and not what a pipe does.
 *
 * WHAT THE ALGORITHM ACTUALLY IS. A loop and four watchers, all of them reactions:
 *   - the LOOP waits on the writer's `ready` (which is the destination's backpressure), reads one chunk, hands
 *     it to `write` without waiting for it, and goes round again. A write's rejection is deliberately ignored
 *     here — the destination's own `closed` promise is what reports it, and reporting it twice would settle
 *     this pipe with an error the spec attributes to the other direction.
 *   - the four WATCHERS are the spec's error-forward, error-backward, close-forward and close-backward rules,
 *     and each is a reaction on the reader's or the writer's `closed`. Whichever fires first calls shutdown;
 *     the rest find `shutting_down` already set and do nothing, which is how the spec makes the first cause
 *     the reported one.
 * Shutdown then has three phases the spec keeps strictly apart: WAIT for the writes already handed over to
 * finish (only while the destination could still accept them), PERFORM one action (abort the destination,
 * cancel the source, close the destination, or the signal's compound of the first two), and FINALIZE — release
 * both locks, drop the abort algorithm, settle this pipe's promise.
 *
 * WHY THE MACHINE IS SHAPED LIKE §5's. One step machine with an OPERATION per entry point, all of them sharing
 * one stage loop: a reaction is an entry, `pipeTo` is an entry, and they converge on the same stages. Every
 * call of the page's code — `read`, `write`, `close`, `abort`, `cancel`, `releaseLock`, and the resolving
 * function that settles the answer — is a request the flow suspends on, so a page whose sink loops forever
 * suspends the pipe rather than driving it to completion.
 *
 * WHAT LIVES ON THE HEAP RECORD RATHER THAN THE MACHINE. Everything that outlives one entry: the two streams,
 * the reader and writer, the signal and the algorithm registered on it, the answer's capability, the
 * shutdown's decision, and the count of writes still outstanding. A machine state is per-entry; the pipe is
 * not. */
#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/dom/abort.h"
#include "core/streams/pipe.h"
#include "core/streams/readable_stream.h"
#include "core/streams/writable_stream.h"
#include "core/streams/stream_work.h"

/* ---- the record ---------------------------------------------------------------------------------------- */

/* WHICH ACTION a shutdown carries. The spec passes a closure; there are exactly four of them and they are
   decided by WHICH RULE fired, so they are an enum. A closure here would be a function object built to be
   called once, and the machine would still have to know which one it was in order to resume in the right
   stage after it suspended. */
enum { ACT_NONE = 0, ACT_ABORT_DEST, ACT_CANCEL_SOURCE, ACT_CLOSE_DEST, ACT_SIGNAL };

/* §4.2.4's options, in the order Web IDL converts them — LEXICOGRAPHIC, which a page pins directly by throwing
   from one getter and counting which others ran. */
enum { OPT_ABORT = 0, OPT_CANCEL, OPT_CLOSE, OPT_SIGNAL, OPT_N };
static const char *const OPT_NAMES[OPT_N] = { "preventAbort", "preventCancel", "preventClose", "signal" };

/* §4.2's `ReadableWritablePair`, likewise lexicographic, and both members are REQUIRED. */
enum { TR_READABLE = 0, TR_WRITABLE, TR_N };
static const char *const TR_NAMES[TR_N] = { "readable", "writable" };

typedef struct {
    JSValue source, dest;
    JSValue reader, writer;
    JSValue signal;        /* the AbortSignal from the options, or undefined */
    JSValue algo;          /* the abort algorithm registered on it, so finalize can take it off again */
    JSValue promise, funcs[2];
    JSValue error;         /* the shutdown's originalError */
    /* HOW MANY WRITES HAVE BEEN HANDED OVER AND NOT YET SETTLED. The spec waits on the LATEST write and then
       re-checks whether a newer one started; §5 settles writes in the order they were queued, so "the latest
       has settled and no newer one exists" and "none is outstanding" are the same instant. A count says which
       one it means and cannot re-enter. */
    uint32_t writes;
    uint8_t prevent[3];    /* indexed by OPT_ABORT / OPT_CANCEL / OPT_CLOSE */
    uint8_t shutting_down;
    uint8_t is_error;
    uint8_t action;
    uint8_t waiting_writes;   /* a shutdown is parked until `writes` reaches zero */
    /* IS A READ OUTSTANDING, AND WHAT RULE IS WAITING BEHIND IT.
       §4.5's PullSteps close the stream BEFORE it performs the read request's chunk steps — but those steps
       are SYNCHRONOUS, so by the time anything can observe `reader.closed` the chunk has already been handed
       over and its write issued. Here the read is a PROMISE, so its reaction is a job and the closed
       reaction's job is enqueued first: the pipe would learn the source had closed while still holding a read
       that was about to deliver the last chunk, and would close the destination out from under it. The chunk
       was written nowhere and no test said so except by counting.
       So a source-side rule that arrives with a read outstanding is HELD until that read is delivered, which
       is the state the spec's synchronous steps guarantee and not a delay invented on top of them. */
    uint8_t read_pending;
    uint8_t deferred_op;      /* 0 = none; otherwise the source-side rule waiting for the read */
    JSValue deferred_err;
    /* the signal's compound action: two operations started together, both awaited, first rejection wins */
    uint8_t acts_pending;
    uint8_t acts_failed;
    JSValue acts_error;
} PipeData;

static JSClassID g_pipe_class;
static JSRuntime *g_pipe_rt;

static PipeData *pipe_of(JSValueConst v) { return JS_GetOpaque(v, g_pipe_class); }

static void pipe_finalizer(JSRuntime *rt, JSValue val)
{
    PipeData *p = JS_GetOpaque(val, g_pipe_class);
    int k;
    if (!p) return;
    JS_FreeValueRT(rt, p->source);   JS_FreeValueRT(rt, p->dest);
    JS_FreeValueRT(rt, p->reader);   JS_FreeValueRT(rt, p->writer);
    JS_FreeValueRT(rt, p->signal);   JS_FreeValueRT(rt, p->algo);
    JS_FreeValueRT(rt, p->promise);  JS_FreeValueRT(rt, p->error);
    JS_FreeValueRT(rt, p->acts_error);   JS_FreeValueRT(rt, p->deferred_err);
    for (k = 0; k < 2; k++) JS_FreeValueRT(rt, p->funcs[k]);
    js_free_rt(rt, p);
}

static void pipe_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    PipeData *p = JS_GetOpaque(val, g_pipe_class);
    int k;
    if (!p) return;
    JS_MarkValue(rt, p->source, mark);   JS_MarkValue(rt, p->dest, mark);
    JS_MarkValue(rt, p->reader, mark);   JS_MarkValue(rt, p->writer, mark);
    JS_MarkValue(rt, p->signal, mark);   JS_MarkValue(rt, p->algo, mark);
    JS_MarkValue(rt, p->promise, mark);  JS_MarkValue(rt, p->error, mark);
    JS_MarkValue(rt, p->acts_error, mark);   JS_MarkValue(rt, p->deferred_err, mark);
    for (k = 0; k < 2; k++) JS_MarkValue(rt, p->funcs[k], mark);
}

/* EMPTY AND ALREADY ATTACHED, like every other record here: a record whose fields are placed before it is
   attached to its object is a record that leaks on any failure in between. */
static JSValue pipe_new(JSContext *ctx)
{
    JSValue obj = JS_NewObjectClass(ctx, g_pipe_class);
    PipeData *p;
    int k;

    if (JS_IsException(obj)) return obj;
    p = js_mallocz(ctx, sizeof *p);
    if (!p) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    p->source = p->dest = p->reader = p->writer = JS_UNDEFINED;
    p->signal = p->algo = p->promise = p->error = JS_UNDEFINED;
    p->acts_error = p->deferred_err = JS_UNDEFINED;
    for (k = 0; k < 2; k++) p->funcs[k] = JS_UNDEFINED;
    JS_SetOpaque(obj, p);
    return obj;
}

/* ---- the machine --------------------------------------------------------------------------------------- */

enum {
    OP_PIPE_TO = 0, OP_PIPE_THROUGH,   /* the two members */
    /* the loop */
    OP_READY_OK, OP_READY_ERR, OP_READ_OK, OP_READ_ERR, OP_WRITE_SETTLED,
    /* the four propagation rules, as reactions on the two `closed` promises */
    OP_SRC_CLOSED, OP_SRC_ERRORED, OP_DST_CLOSED, OP_DST_ERRORED,
    /* an action's answer, and the signal */
    OP_ACT_OK, OP_ACT_ERR, OP_ABORT_ALGO,
    OP_N
};

/* WHERE THIS MACHINE RESTS, AS §4.2.4 NUMBERS IT. `pipeTo` and `pipeThrough` are four and six steps whose last
   real one is ReadableStreamPipeTo, and that operation's step 15 is prose rather than a numbered list — it
   names its parts ("error and close states must be propagated", "shutdown with an action", "finalize"), so the
   labels name them the way the standard writes them. Everything before step 15 IS numbered and the stages say
   which number they are at. */
#define PIPE_STAGES(X) \
    X(S_ENTRY, "Streams §4.2 pipeTo/pipeThrough (this invocation's entry: which member or which of §4.2.4's " \
               "reactions it is)") \
    X(S_TRANSFORM, "Streams §4.2 pipeThrough(transform, options) steps 2 and 4 (Get(transform, \"writable\") " \
                   "and Get(transform, \"readable\"))") \
    X(S_OPT, "Streams §4.2 pipeTo step 3 / pipeThrough step 3 (the StreamPipeOptions members, read in the " \
             "lexicographic order Web IDL §3.2.17 converts a dictionary in)") \
    X(S_LOCKS, "Streams §4.2 pipeTo steps 1-2 / pipeThrough steps 1-2 (the two lock checks — a rejected " \
               "promise for pipeTo, a throw for pipeThrough)") \
    X(S_ACQ_READER, "Streams §4.2.4 ReadableStreamPipeTo step 9 (AcquireReadableStreamDefaultReader on the " \
                    "source)") \
    X(S_ACQ_WRITER, "Streams §4.2.4 ReadableStreamPipeTo step 10 (AcquireWritableStreamDefaultWriter on the " \
                    "destination)") \
    X(S_WIRE, "Streams §4.2.4 ReadableStreamPipeTo steps 11-14 (the pipe's promise, the abort algorithm, and " \
              "step 15's \"error and close states must be propagated\" checks in the order it lists them)") \
    X(S_LOOP, "Streams §4.2.4 ReadableStreamPipeTo step 15's backpressure rule (wait on the writer's `ready` " \
              "before reading again)") \
    X(S_READ, "Streams §4.2.4 ReadableStreamPipeTo step 15 (read a chunk from the reader)") \
    X(S_READ_DONE, "Streams §4.2.4 ReadableStreamPipeTo step 15 (the read's result: a chunk to write, or the " \
                   "source's close)") \
    X(S_WRITE, "Streams §4.2.4 ReadableStreamPipeTo step 15 (write the chunk to the destination, without " \
               "waiting for it)") \
    X(S_DEFERRED, "Streams §4.2.4 ReadableStreamPipeTo step 15's propagation rules (the source-side rule that " \
                  "was waiting for a read in flight to be delivered)") \
    X(S_SHUTDOWN, "Streams §4.2.4 ReadableStreamPipeTo step 15's \"shutdown\" (whether the pending writes must " \
                  "settle first)") \
    X(S_ACT_PICK, "Streams §4.2.4 ReadableStreamPipeTo step 15's \"shutdown with an action\" (choosing the " \
                  "action: abort the destination, cancel the source, or close the destination)") \
    X(S_ACTION, "Streams §4.2.4 ReadableStreamPipeTo step 15's \"shutdown with an action\" (performing it)") \
    X(S_SIG_PICK, "Streams §4.2.4 ReadableStreamPipeTo step 14.1 (the abort algorithm's ordered set of " \
                  "actions, chosen from preventAbort and preventCancel)") \
    X(S_SIG_CALL, "Streams §4.2.4 ReadableStreamPipeTo step 14.1.5 (waiting for all of those actions)") \
    X(S_FINALIZE, "Streams §4.2.4 ReadableStreamPipeTo step 15's \"finalize\" (release the writer)") \
    X(S_REL_READER, "Streams §4.2.4 ReadableStreamPipeTo step 15's \"finalize\" (release the reader)") \
    X(S_SETTLE, "Streams §4.2.4 ReadableStreamPipeTo step 15's \"finalize\" (settle the pipe's promise — the " \
                "resolving function's 27.5.1.3 step 2.f `then` read is the page's)") \
    X(S_RESULT, "Streams §4.2 pipeTo steps 1-2 (settling this member's OWN capability, for the answers it " \
                "rejects with before any pipe exists)") \
    X(S_DONE, "Streams §4.2 pipeTo step 4 / pipeThrough steps 5-6 (the operation is complete)")
enum { PIPE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PIPE_STEPS[] = { PIPE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    StreamWork w;       /* the stage, whichever call is in flight, and its buffer */
    JSValue pipe;       /* the record (owned) */
    JSValue value;      /* the read result / the settled value / the option being read (owned) */
    JSValue tr[TR_N];   /* pipeThrough's `readable` and `writable` (owned) */
    JSValue sig;        /* the `signal` option, until the record exists to hold it (owned) */
    /* THE ACTION, RESOLVED. Which function on which receiver was decided at the picker stage, so the stage
       that issues the call has nothing left to decide and cannot decide it differently on the way back in. */
    JSValue act_fn, act_recv;
    uint8_t act_argc;
    JSValue promise, funcs[2];   /* this member's own answer, for the paths that reject before the pipe exists */
    /* THE OPTIONS ARE READ BEFORE THE RECORD EXISTS, because a throwing getter must answer the page and leave
       no pipe behind at all — so they land here first and are copied over once the locks have been checked. */
    uint8_t prevent[3];
    uint8_t member;
    uint8_t reject;
    uint8_t through;    /* this entry is pipeThrough, so the answer is `readable` and a failure THROWS */
} JSPipeState;

static int g_op_stepid[OP_N];
static int g_pipe_to_stepid = -1, g_pipe_through_stepid = -1;

static void js_pipe_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSPipeState *s = st;
    int k;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->pipe);
    v->val(ctx, &s->value);
    for (k = 0; k < TR_N; k++) v->val(ctx, &s->tr[k]);
    v->val(ctx, &s->sig);
    v->val(ctx, &s->act_fn);
    v->val(ctx, &s->act_recv);
    v->val(ctx, &s->promise);
    for (k = 0; k < 2; k++) v->val(ctx, &s->funcs[k]);
}

/* DELETED: js_pipe_release, which restated js_pipe_visit field for field. */
/* The member's ANSWER: pipeTo's promise, or pipeThrough's `readable`. A reaction has none — its answer is the
   settling it performed — so an undefined here is the right result for one. */
static JSValue js_pipe_fini(JSContext *ctx, void *st, bool take_result)
{
    JSPipeState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    (void)ctx;
    if (take_result) s->promise = JS_UNDEFINED;
    return r;
}

/* Attach one reaction pair to `promise`, both entries re-entering this machine with the record. */
static int pipe_react(JSContext *ctx, JSValueConst promise, int ok_op, int err_op, JSValueConst pipe)
{
    return stream_react(ctx, promise, g_op_stepid[ok_op], g_op_stepid[err_op], &pipe, 1);
}

/* A member's short-circuit answer: its own capability, settled at S_RESULT. §4.2's `pipeTo` REJECTS where it
   would otherwise throw, because Web IDL makes a promise-returning operation answer that way — a brand
   failure, a throwing option getter and a locked stream all arrive at the page as a rejection. */
static int pipe_short(JSContext *ctx, JSPipeState *s, int reject, JSValue value)
{
    s->promise = JS_NewPromiseCapability(ctx, s->funcs);
    if (JS_IsException(s->promise)) { JS_FreeValue(ctx, value); return -1; }
    JS_FreeValue(ctx, s->w.value);
    s->w.value = value;
    s->reject = (uint8_t)reject;
    STEP_GOTO(s->hdr.stage, S_RESULT, &s->w.phase, NULL);
    return 0;
}

/* THE ONE PLACE A SHUTDOWN BEGINS. Every rule reaches it, and the first one to arrive is the one that decides
   what the pipe reports — which is exactly what `shutting_down` means. `error` is CONSUMED. */
static void pipe_begin_shutdown(JSContext *ctx, JSPipeState *s, PipeData *p, int is_error, JSValue error,
                                int action)
{
    if (p->shutting_down) {
        JS_FreeValue(ctx, error);
        STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
        return;
    }
    p->shutting_down = 1;
    p->is_error = (uint8_t)is_error;
    p->action = (uint8_t)action;
    JS_FreeValue(ctx, p->error);
    p->error = error;
    STEP_GOTO(s->hdr.stage, S_SHUTDOWN, &s->w.phase, NULL);
}

/* §4.2.4's two SOURCE-side rules, applied from the one place both reach — whether at once or after the read
   they were waiting behind. */
static void pipe_apply_source_rule(JSContext *ctx, JSPipeState *s, PipeData *p, int op, JSValueConst err)
{
    if (op == OP_SRC_ERRORED)
        pipe_begin_shutdown(ctx, s, p, 1, JS_DupValue(ctx, err),          /* rule 1: errors forward */
                            p->prevent[OPT_ABORT] ? ACT_NONE : ACT_ABORT_DEST);
    else
        pipe_begin_shutdown(ctx, s, p, 0, JS_UNDEFINED,                   /* rule 3: closing forward */
                            p->prevent[OPT_CLOSE] ? ACT_NONE : ACT_CLOSE_DEST);
}

static int js_pipe_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSPipeState *s = st;
    JSStepHdr *hdr = &s->hdr;
    int op = hdr->arg;
    PipeData *p;
    JSValue out;
    int r;

    if (s->hdr.stage == S_ENTRY) {
        stream_work_start(&s->w);
        s->pipe = s->value = s->sig = s->act_fn = s->act_recv = s->promise = JS_UNDEFINED;
        s->funcs[0] = s->funcs[1] = JS_UNDEFINED;
        s->tr[TR_READABLE] = s->tr[TR_WRITABLE] = JS_UNDEFINED;
        s->prevent[0] = s->prevent[1] = s->prevent[2] = 0;
        s->member = 0;
        s->reject = 0;
        s->through = (op == OP_PIPE_THROUGH);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        if (op > OP_PIPE_THROUGH) {
            /* a REACTION: the record is what it captured, and the settled value is its argument. */
            s->pipe = JS_DupValue(ctx, JS_StepClosureData(hdr, 0));
            s->value = JS_DupValue(ctx, step_arg(hdr, 0));
            p = pipe_of(s->pipe);
            DCHECK(p != NULL, "a §4.2.4 reaction captured something that is not a pipe");
            switch (op) {
            case OP_READY_OK:
                /* backpressure has cleared: read the next chunk — unless the pipe is already stopping, in
                   which case the loop simply ends here and shutdown owns what happens next. */
                STEP_GOTO(s->hdr.stage, p->shutting_down ? S_DONE : S_READ, &s->w.phase, NULL);
                break;
            case OP_READ_ERR:
                /* a rejected read means the SOURCE failed, which its `closed` watcher reports — this only ends
                   the loop. The read is delivered either way, so a rule held behind it may now run. */
                p->read_pending = 0;
                STEP_GOTO(s->hdr.stage, p->deferred_op ? S_DEFERRED : S_DONE, &s->w.phase, NULL);
                break;
            case OP_READY_ERR:
                /* THE LOOP STOPS AND REPORTS NOTHING. A rejected `ready` means the destination failed and a
                   rejected read means the source did; each of those has its own watcher on the matching
                   `closed` promise, and that watcher is what the spec makes responsible for the shutdown.
                   Reporting it here as well would settle the pipe twice over. */
                STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
                break;
            case OP_READ_OK:
                p->read_pending = 0;
                STEP_GOTO(s->hdr.stage, S_READ_DONE, &s->w.phase, NULL);
                break;
            case OP_WRITE_SETTLED:
                DCHECK(p->writes > 0, "a §4.2.4 write settled that was never counted as outstanding");
                p->writes--;
                /* the wait that shutdown parked on is over exactly when the last one settles */
                /* THE FLAG IS CLEARED IN THE ARM THAT ENDS THE WAIT. It was cleared by a line testing the
                   stage against S_ACTION immediately after that same line had set it to S_ACT_PICK or
                   S_DONE — a condition that cannot hold, so the flag survived a shutdown it had already
                   released. Naming the stages made it visible; nothing about the numbers could. */
                if (p->waiting_writes && p->writes == 0) {
                    p->waiting_writes = 0;
                    STEP_GOTO(s->hdr.stage, S_ACT_PICK, &s->w.phase, NULL);
                } else {
                    STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
                }
                break;
            case OP_SRC_ERRORED:
            case OP_SRC_CLOSED:
                /* §4.2.4 rules 1 and 3 — the SOURCE side. Both are held while a read is outstanding, for the
                   reason `read_pending` states: the spec cannot observe either of them before the read it
                   already answered has delivered. */
                if (p->read_pending) {
                    p->deferred_op = (uint8_t)op;
                    JS_FreeValue(ctx, p->deferred_err);
                    p->deferred_err = JS_DupValue(ctx, s->value);
                    STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
                    break;
                }
                pipe_apply_source_rule(ctx, s, p, op, s->value);
                break;
            case OP_DST_ERRORED:
                /* §4.2.4 rule 2, errors backward. */
                pipe_begin_shutdown(ctx, s, p, 1, JS_DupValue(ctx, s->value),
                                    p->prevent[OPT_CANCEL] ? ACT_NONE : ACT_CANCEL_SOURCE);
                break;
            case OP_DST_CLOSED: {
                /* §4.2.4 rule 4, closing backward: the destination finished under the pipe, which is an ERROR
                   for the pipe however ordinary it was for the destination — there is still source left. */
                JSValue err;
                JS_ThrowTypeError(ctx, "the destination writable stream closed before all data could be piped "
                                       "to it");
                err = JS_GetException(ctx);
                pipe_begin_shutdown(ctx, s, p, 1, err, p->prevent[OPT_CANCEL] ? ACT_NONE : ACT_CANCEL_SOURCE);
                break;
            }
            case OP_ACT_OK: case OP_ACT_ERR: {
                /* §4.2.4: when the ACTION fails, that failure is what the pipe reports, whatever the rule that
                   started the shutdown was going to report. */
                int failed = (op == OP_ACT_ERR);
                if (p->action != ACT_SIGNAL) {
                    if (failed) {
                        p->is_error = 1;
                        JS_FreeValue(ctx, p->error);
                        p->error = JS_DupValue(ctx, s->value);
                    }
                    STEP_GOTO(s->hdr.stage, S_FINALIZE, &s->w.phase, NULL);
                    break;
                }
                /* THE SIGNAL'S COMPOUND IS A Promise.all, so it answers only when BOTH have settled and it
                   reports the FIRST rejection. Finalizing on the first fulfilment would release the locks
                   while the other operation was still running against them. */
                DCHECK(p->acts_pending > 0, "a §4.2.4 compound action settled more times than it was started");
                p->acts_pending--;
                if (failed && !p->acts_failed) {
                    p->acts_failed = 1;
                    JS_FreeValue(ctx, p->acts_error);
                    p->acts_error = JS_DupValue(ctx, s->value);
                }
                if (p->acts_pending) { STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL); break; }
                if (p->acts_failed) {
                    p->is_error = 1;
                    JS_FreeValue(ctx, p->error);
                    p->error = p->acts_error;
                    p->acts_error = JS_UNDEFINED;
                }
                STEP_GOTO(s->hdr.stage, S_FINALIZE, &s->w.phase, NULL);
                break;
            }
            default: {
                /* §4.2.4 step 5's abort algorithm: the signal fired. */
                JSValue reason;
                DCHECK(op == OP_ABORT_ALGO, "a §4.2.4 machine ran with an operation this component does not have");
                reason = abort_signal_reason(ctx, p->signal);
                pipe_begin_shutdown(ctx, s, p, 1, reason, ACT_SIGNAL);
                break;
            }
            }
            goto run;
        }

        /* A MEMBER. Web IDL brand-checks the receiver and then converts the arguments in order; every failure
           on this path is the page's answer, and for `pipeTo` that answer is a REJECTED PROMISE. */
        if (!readable_stream_is(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "not a ReadableStream");
            if (s->through) return JS_STEP_ABRUPT;
            if (pipe_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
            goto run;
        }
        STEP_GOTO(s->hdr.stage, s->through ? S_TRANSFORM : S_OPT, &s->w.phase, NULL);
    }

run:
    p = JS_IsUndefined(s->pipe) ? NULL : pipe_of(s->pipe);

    /* CATCHES_ABRUPT: A REQUEST THAT THREW COMES BACK AS A VALUE. The trampoline hands this machine
       `JS_EXCEPTION` with the throw still live rather than unwinding it — which is what lets `pipeTo` answer a
       throwing option getter with a REJECTION, as Web IDL requires of a promise-returning operation. The call
       requests below see it in their own `out` and handle it there; the KEYED READS cannot, because
       step_getprop_run's contract has no way to say "it threw", so it simply reported "not started" and the
       machine asked for the same property again, forever. */
    if (JS_IsException(cb_result) && (s->hdr.stage == S_TRANSFORM || s->hdr.stage == S_OPT)) {
        cb_result = JS_UNDEFINED;
        /* `pipeThrough` returns a ReadableStream, so its answer is the throw itself; `pipeTo` returns a
           promise, so its answer is that promise rejected. */
        if (s->through) return JS_STEP_ABRUPT;
        if (pipe_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
    }

    for (;;) {
        switch (s->hdr.stage) {

        case S_TRANSFORM:
            /* §4.2's `ReadableWritablePair`: two REQUIRED members, read in lexicographic order off whatever
               the page passed — so each read is a request, because either can be an accessor. */
            while (s->member < TR_N) {
                JSAtom a;
                if (!JS_IsObject(hdr->argv[0])) {
                    JS_ThrowTypeError(ctx, "the transform must be an object with `readable` and `writable`");
                    return JS_STEP_ABRUPT;
                }
                a = JS_NewAtom(ctx, TR_NAMES[s->member]);
                r = step_getprop_run(ctx, hdr, hdr->argv[0], a, cb_result, &s->tr[s->member],
                                     out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
                /* EACH MEMBER IS BRAND-CHECKED AS IT ARRIVES, not both at the end. Web IDL converts a
                   dictionary member's TYPE the moment it has read it, so a `readable` that is not a
                   ReadableStream is a TypeError BEFORE `writable` is even asked for — and a page counts that
                   by giving `writable` a getter and asserting it never ran. */
                if ((s->member == TR_READABLE && !readable_stream_is(s->tr[TR_READABLE])) ||
                    (s->member == TR_WRITABLE && !writable_stream_is(s->tr[TR_WRITABLE]))) {
                    JS_ThrowTypeError(ctx, "the transform's `%s` must be a stream", TR_NAMES[s->member]);
                    return JS_STEP_ABRUPT;
                }
                s->member++;
            }
            s->member = 0;
            STEP_GOTO(s->hdr.stage, S_OPT, &s->w.phase, NULL);
            continue;

        case S_OPT: {
            /* BOTH MEMBERS TAKE THE OPTIONS SECOND — `pipeTo(destination, options)` and
               `pipeThrough(transform, options)`. What differs is only where the DESTINATION comes from. */
            JSValueConst opts = step_arg(hdr, 1);
            JSValueConst dest = s->through ? (JSValueConst)s->tr[TR_WRITABLE] : step_arg(hdr, 0);

            /* THE DESTINATION IS CONVERTED BEFORE THE OPTIONS, because Web IDL converts arguments in order and
               a page pins that by passing a bad destination together with a throwing option getter. */
            if (!s->through && !writable_stream_is(dest)) {
                JS_ThrowTypeError(ctx, "pipeTo's destination must be a WritableStream");
                if (pipe_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            while (s->member < OPT_N && JS_IsObject(opts)) {
                JSAtom a = JS_NewAtom(ctx, OPT_NAMES[s->member]);
                JS_FreeValue(ctx, s->value);
                s->value = JS_UNDEFINED;
                r = step_getprop_run(ctx, hdr, opts, a, cb_result, &s->value, out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                if (r < 0) {
                    /* A THROWING GETTER IS THE PAGE'S ANSWER, and which answer depends on the member's return
                       type: `pipeThrough` returns a ReadableStream and throws, `pipeTo` returns a promise and
                       rejects. Web IDL decides that, not this algorithm. */
                    if (s->through) return JS_STEP_ABRUPT;
                    if (pipe_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                    goto opt_done;
                }
                if (s->member == OPT_SIGNAL) {
                    /* `AbortSignal signal` — not nullable and with no default, so an ABSENT member is absent
                       and anything present that is not a signal is a TypeError. */
                    if (!JS_IsUndefined(s->value) && !abort_signal_is(ctx, s->value)) {
                        JS_ThrowTypeError(ctx, "pipeTo's `signal` option must be an AbortSignal");
                        if (s->through) return JS_STEP_ABRUPT;
                        if (pipe_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                        goto opt_done;
                    }
                    JS_FreeValue(ctx, s->sig);
                    s->sig = JS_DupValue(ctx, s->value);
                } else {
                    /* `boolean` — ToBoolean, which runs nothing and cannot fail. */
                    s->prevent[s->member] = (uint8_t)JS_ToBool(ctx, s->value);
                }
                s->member++;
            }
            STEP_GOTO(s->hdr.stage, S_LOCKS, &s->w.phase, NULL);
        opt_done:
            continue;
        }

        case S_LOCKS: {
            JSValueConst dest = s->through ? (JSValueConst)s->tr[TR_WRITABLE] : step_arg(hdr, 0);
            bool src_locked = false, dst_locked = false;

            readable_stream_query(hdr->this_val, NULL, &src_locked);
            writable_stream_query(dest, NULL, &dst_locked, NULL);
            if (src_locked || dst_locked) {
                JS_ThrowTypeError(ctx, src_locked ? "the source stream is locked to a reader"
                                                  : "the destination stream is locked to a writer");
                if (s->through) return JS_STEP_ABRUPT;
                if (pipe_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            s->pipe = pipe_new(ctx);
            if (JS_IsException(s->pipe)) return JS_STEP_ABRUPT;
            p = pipe_of(s->pipe);
            p->source = JS_DupValue(ctx, hdr->this_val);
            p->dest = JS_DupValue(ctx, dest);
            p->signal = JS_DupValue(ctx, s->sig);
            p->prevent[OPT_ABORT] = s->prevent[OPT_ABORT];
            p->prevent[OPT_CANCEL] = s->prevent[OPT_CANCEL];
            p->prevent[OPT_CLOSE] = s->prevent[OPT_CLOSE];
            p->promise = JS_NewPromiseCapability(ctx, p->funcs);
            if (JS_IsException(p->promise)) return JS_STEP_ABRUPT;
            STEP_GOTO(s->hdr.stage, S_ACQ_READER, &s->w.phase, NULL);
            continue;
        }

        case S_ACQ_READER: {
            JSValue op = readable_stream_op(ctx, RS_OP_GET_READER);
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                              p->source, 0, NULL, cb_result, &out, out_cb, out_argc);
            JS_FreeValue(ctx, op);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            p->reader = out;
            STEP_GOTO(s->hdr.stage, S_ACQ_WRITER, &s->w.phase, NULL);
            continue;
        }

        case S_ACQ_WRITER:
            {
                JSValue op = writable_stream_op(ctx, WS_OP_GET_WRITER);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                  p->dest, 0, NULL, cb_result, &out, out_cb, out_argc);
                JS_FreeValue(ctx, op);
            }
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            p->writer = out;
            STEP_GOTO(s->hdr.stage, S_WIRE, &s->w.phase, NULL);
            continue;

        case S_WIRE: {
            ReadableStreamState ss = RS_READABLE;
            WritableStreamState ds = WS_WRITABLE;
            bool close_queued = false;
            JSValue rc, wc;

            /* THE ANSWER IS HANDED OVER NOW, before anything can fire. `pipeTo` returns this promise and
               `pipeThrough` returns the transform's readable while marking it handled, and both of those are
               decided before the first reaction runs. */
            s->promise = s->through ? JS_DupValue(ctx, s->tr[TR_READABLE]) : JS_DupValue(ctx, p->promise);
            if (s->through) JS_MarkPromiseHandled(ctx, p->promise);

            /* §4.2.4 step 5: the signal. An ALGORITHM, not a listener — it runs before the `abort` event and
               the page can neither see it nor remove it. A signal already aborted shuts the pipe down at once
               and the loop never starts. */
            if (!JS_IsUndefined(p->signal)) {
                p->algo = JS_NewStepClosure(ctx, g_op_stepid[OP_ABORT_ALGO], 0, 1, (JSValueConst *)&s->pipe);
                if (JS_IsException(p->algo)) return JS_STEP_ABRUPT;
                if (abort_signal_aborted(ctx, p->signal)) {
                    pipe_begin_shutdown(ctx, s, p, 1, abort_signal_reason(ctx, p->signal), ACT_SIGNAL);
                    continue;
                }
                abort_signal_add_algorithm(ctx, p->signal, p->algo);
            }

            rc = readable_reader_closed(ctx, p->reader);
            wc = writable_writer_closed(ctx, p->writer);
            r = pipe_react(ctx, rc, OP_SRC_CLOSED, OP_SRC_ERRORED, s->pipe);
            if (r == 0) r = pipe_react(ctx, wc, OP_DST_CLOSED, OP_DST_ERRORED, s->pipe);
            JS_FreeValue(ctx, rc);
            JS_FreeValue(ctx, wc);
            if (r < 0) return JS_STEP_ABRUPT;

            /* …and the four rules again for a stream that is ALREADY in the state the watcher waits for. The
               spec runs these synchronously and IN THIS ORDER, so a source that is already errored beats a
               destination that is already closed. The watchers above will also fire, and find the shutdown
               already begun. */
            readable_stream_query(p->source, &ss, NULL);
            writable_stream_query(p->dest, &ds, NULL, &close_queued);
            if (ss == RS_ERRORED) {
                pipe_begin_shutdown(ctx, s, p, 1, readable_stream_stored_error(ctx, p->source),
                                    p->prevent[OPT_ABORT] ? ACT_NONE : ACT_ABORT_DEST);
                continue;
            }
            if (ds == WS_ERRORED || ds == WS_ERRORING) {
                pipe_begin_shutdown(ctx, s, p, 1, writable_stream_stored_error(ctx, p->dest),
                                    p->prevent[OPT_CANCEL] ? ACT_NONE : ACT_CANCEL_SOURCE);
                continue;
            }
            if (ss == RS_CLOSED) {
                pipe_begin_shutdown(ctx, s, p, 0, JS_UNDEFINED,
                                    p->prevent[OPT_CLOSE] ? ACT_NONE : ACT_CLOSE_DEST);
                continue;
            }
            if (close_queued || ds == WS_CLOSED) {
                JSValue err;
                JS_ThrowTypeError(ctx, "the destination writable stream closed before all data could be piped "
                                       "to it");
                err = JS_GetException(ctx);
                pipe_begin_shutdown(ctx, s, p, 1, err, p->prevent[OPT_CANCEL] ? ACT_NONE : ACT_CANCEL_SOURCE);
                continue;
            }
            STEP_GOTO(s->hdr.stage, S_LOOP, &s->w.phase, NULL);
            continue;
        }

        case S_LOOP: {
            /* ONE TURN OF THE LOOP IS ONE REACTION. Waiting on the writer's `ready` IS waiting for the
               destination's backpressure to clear, which is the whole of the flow control. */
            JSValue ready;
            if (p->shutting_down) { STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL); continue; }
            ready = writable_writer_ready(ctx, p->writer);
            r = pipe_react(ctx, ready, OP_READY_OK, OP_READY_ERR, s->pipe);
            JS_FreeValue(ctx, ready);
            if (r < 0) return JS_STEP_ABRUPT;
            STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
            continue;
        }

        case S_READ: {
            JSValue op = readable_stream_op(ctx, RS_OP_READ);
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op, p->reader,
                              0, NULL, cb_result, &out, out_cb, out_argc);
            JS_FreeValue(ctx, op);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            r = pipe_react(ctx, out, OP_READ_OK, OP_READ_ERR, s->pipe);
            JS_FreeValue(ctx, out);
            if (r < 0) return JS_STEP_ABRUPT;
            p->read_pending = 1;
            STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
            continue;
        }

        case S_READ_DONE: {
            /* The read result is 7.4.14's `{ value, done }`, built by §4.3 for this read alone — so its two
               members are read as OWN SLOTS. It is an ordinary object and the page owns Object.prototype;
               going through a lookup would let a page trap a value it has never been handed. */
            JSValue done_v = JS_UNDEFINED, chunk = JS_UNDEFINED;
            JSAtom a_done = JS_NewAtom(ctx, "done"), a_value = JS_NewAtom(ctx, "value");
            int done;
            if (JS_GetOwnSlot(ctx, &done_v, s->value, a_done) <= 0) done_v = JS_UNDEFINED;
            if (JS_GetOwnSlot(ctx, &chunk, s->value, a_value) <= 0) chunk = JS_UNDEFINED;
            JS_FreeAtom(ctx, a_done);
            JS_FreeAtom(ctx, a_value);
            done = JS_ToBool(ctx, done_v);
            JS_FreeValue(ctx, done_v);
            if (done) {
                /* the close-forward rule owns what happens next, through the reader's `closed` watcher */
                JS_FreeValue(ctx, chunk);
                STEP_GOTO(s->hdr.stage, p->deferred_op ? S_DEFERRED : S_DONE, &s->w.phase, NULL);
                continue;
            }
            JS_FreeValue(ctx, s->w.value);
            s->w.value = chunk;
            STEP_GOTO(s->hdr.stage, S_WRITE, &s->w.phase, NULL);
            continue;
        }

        case S_WRITE: {
            JSValueConst arg = s->w.value;
            {
                JSValue op = writable_stream_op(ctx, WS_OP_WRITE);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op, p->writer,
                                  1, &arg, cb_result, &out, out_cb, out_argc);
                JS_FreeValue(ctx, op);
            }
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            /* THE PIPE DOES NOT WAIT FOR THE WRITE, and does not report its failure: the destination's own
               `closed` promise is what reports that, and the loop goes round as soon as `ready` allows. What
               it DOES track is that the write is outstanding, because a shutdown must let it finish. */
            p->writes++;
            r = pipe_react(ctx, out, OP_WRITE_SETTLED, OP_WRITE_SETTLED, s->pipe);
            JS_FreeValue(ctx, out);
            if (r < 0) return JS_STEP_ABRUPT;
            /* THE WRITE IS ISSUED BEFORE THE HELD RULE RUNS, which is the whole point of holding it: the
               destination now has the chunk, so the shutdown's wait-for-writes covers it. */
            STEP_GOTO(s->hdr.stage, p->deferred_op ? S_DEFERRED : S_LOOP, &s->w.phase, NULL);
            continue;
        }

        case S_DEFERRED: {
            int held = p->deferred_op;
            JSValue err = p->deferred_err;
            DCHECK(held != 0, "a §4.2.4 machine reached the held-rule stage with no rule held");
            p->deferred_op = 0;
            p->deferred_err = JS_UNDEFINED;
            pipe_apply_source_rule(ctx, s, p, held, err);
            JS_FreeValue(ctx, err);
            continue;
        }

        case S_SHUTDOWN: {
            /* WAIT FOR THE WRITES ALREADY HANDED OVER, but only while the destination could still take them:
               once it is closing or has failed, nothing more will settle and waiting would never end. */
            WritableStreamState ds = WS_WRITABLE;
            bool close_queued = false;
            writable_stream_query(p->dest, &ds, NULL, &close_queued);
            if (ds == WS_WRITABLE && !close_queued && p->writes > 0) {
                p->waiting_writes = 1;
                STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
                continue;
            }
            STEP_GOTO(s->hdr.stage, S_ACT_PICK, &s->w.phase, NULL);
            continue;
        }

        case S_ACT_PICK: {
            /* WHICH operation this shutdown performs, decided ONCE. Nothing here calls anything, which is what
               makes it safe to branch on state the calls below will change. */
            if (p->action == ACT_NONE) { STEP_GOTO(s->hdr.stage, S_FINALIZE, &s->w.phase, NULL); continue; }
            if (p->action == ACT_SIGNAL) {
                p->acts_pending = 0;
                p->acts_failed = 0;
                JS_FreeValue(ctx, p->acts_error);
                p->acts_error = JS_UNDEFINED;
                s->member = 0;
                STEP_GOTO(s->hdr.stage, S_SIG_PICK, &s->w.phase, NULL);
                continue;
            }
            JS_FreeValue(ctx, s->act_fn);
            JS_FreeValue(ctx, s->act_recv);
            s->act_argc = 1;
            if (p->action == ACT_ABORT_DEST) {
                s->act_fn = writable_stream_op(ctx, WS_OP_ABORT);
                s->act_recv = JS_DupValue(ctx, p->writer);
            } else if (p->action == ACT_CANCEL_SOURCE) {
                s->act_fn = readable_stream_op(ctx, RS_OP_CANCEL);
                s->act_recv = JS_DupValue(ctx, p->reader);
            } else {
                /* WritableStreamDefaultWriterCloseWithErrorPropagation: a destination that is already closing
                   or closed answers at once, an errored one answers with its own error, and only a live one is
                   actually closed. Without those three steps a pipe whose source closed after the destination
                   had begun closing would ask it to close a second time and get a TypeError it then reported
                   as the pipe's own failure. */
                WritableStreamState ds = WS_WRITABLE;
                bool close_queued = false;
                DCHECK(p->action == ACT_CLOSE_DEST, "a §4.2.4 shutdown carried an action this file does not have");
                s->act_fn = s->act_recv = JS_UNDEFINED;
                writable_stream_query(p->dest, &ds, NULL, &close_queued);
                if (close_queued || ds == WS_CLOSED) {
                    STEP_GOTO(s->hdr.stage, S_FINALIZE, &s->w.phase, NULL);
                    continue;
                }
                if (ds == WS_ERRORED) {
                    p->is_error = 1;
                    JS_FreeValue(ctx, p->error);
                    p->error = writable_stream_stored_error(ctx, p->dest);
                    STEP_GOTO(s->hdr.stage, S_FINALIZE, &s->w.phase, NULL);
                    continue;
                }
                s->act_fn = writable_stream_op(ctx, WS_OP_CLOSE);
                s->act_recv = JS_DupValue(ctx, p->writer);
                s->act_argc = 0;
            }
            STEP_GOTO(s->hdr.stage, S_ACTION, &s->w.phase, NULL);
            continue;
        }

        case S_ACTION: {
            JSValueConst arg = p->error;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->act_fn, s->act_recv, s->act_argc, &arg,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            r = pipe_react(ctx, out, OP_ACT_OK, OP_ACT_ERR, s->pipe);
            JS_FreeValue(ctx, out);
            if (r < 0) return JS_STEP_ABRUPT;
            STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
            continue;
        }

        case S_SIG_PICK: {
            /* §4.2.4 step 5's compound: up to two operations, each guarded by the state its own rule names,
               all STARTED before either is awaited — a page counts which of its `abort` and `cancel` ran
               before either settled, so this is not a chain. The guard is read here and the call is issued in
               the next stage, for the reason above: starting the abort is what stops the destination being
               writable, and re-reading that on the way back in would abandon the call. */
            bool run_it;
            if (s->member >= 2) {
                /* Neither ran — Promise.all of nothing fulfils at once. */
                STEP_GOTO(s->hdr.stage, p->acts_pending ? S_DONE : S_FINALIZE, &s->w.phase, NULL);
                continue;
            }
            JS_FreeValue(ctx, s->act_fn);
            JS_FreeValue(ctx, s->act_recv);
            if (s->member == 0) {
                WritableStreamState ds = WS_WRITABLE;
                writable_stream_query(p->dest, &ds, NULL, NULL);
                run_it = !p->prevent[OPT_ABORT] && ds == WS_WRITABLE;
                s->act_fn = writable_stream_op(ctx, WS_OP_ABORT);
                s->act_recv = JS_DupValue(ctx, p->writer);
            } else {
                ReadableStreamState ss = RS_READABLE;
                readable_stream_query(p->source, &ss, NULL);
                run_it = !p->prevent[OPT_CANCEL] && ss == RS_READABLE;
                s->act_fn = readable_stream_op(ctx, RS_OP_CANCEL);
                s->act_recv = JS_DupValue(ctx, p->reader);
            }
            s->act_argc = 1;
            if (!run_it) { s->member++; continue; }
            STEP_GOTO(s->hdr.stage, S_SIG_CALL, &s->w.phase, NULL);
            continue;
        }

        case S_SIG_CALL: {
            JSValueConst arg = p->error;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->act_fn, s->act_recv, s->act_argc, &arg,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            p->acts_pending++;
            r = pipe_react(ctx, out, OP_ACT_OK, OP_ACT_ERR, s->pipe);
            JS_FreeValue(ctx, out);
            if (r < 0) return JS_STEP_ABRUPT;
            s->member++;
            STEP_GOTO(s->hdr.stage, S_SIG_PICK, &s->w.phase, NULL);
            continue;
        }

        case S_FINALIZE:
            /* §4.2.4's finalize: release the writer's lock, then the reader's, then answer. */
            {
                JSValue op = writable_stream_op(ctx, WS_OP_RELEASE);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op, p->writer,
                                  0, NULL, cb_result, &out, out_cb, out_argc);
                JS_FreeValue(ctx, op);
            }
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            STEP_GOTO(s->hdr.stage, S_REL_READER, &s->w.phase, NULL);
            continue;

        case S_REL_READER: {
            JSValue op = readable_stream_op(ctx, RS_OP_RELEASE);
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op, p->reader,
                              0, NULL, cb_result, &out, out_cb, out_argc);
            JS_FreeValue(ctx, op);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            if (!JS_IsUndefined(p->signal) && !JS_IsUndefined(p->algo))
                abort_signal_remove_algorithm(ctx, p->signal, p->algo);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = p->is_error ? JS_DupValue(ctx, p->error) : JS_UNDEFINED;
            JS_FreeValue(ctx, s->w.func);
            s->w.func = JS_DupValue(ctx, p->funcs[p->is_error]);
            STEP_GOTO(s->hdr.stage, S_SETTLE, &s->w.phase, NULL);
            continue;
        }

        case S_SETTLE: {
            JSValueConst arg = s->w.value;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, JS_UNDEFINED, 1, &arg, cb_result,
                              &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
            continue;
        }

        case S_RESULT: {
            JSValueConst arg = s->w.value;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->funcs[s->reject], JS_UNDEFINED, 1, &arg,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            STEP_GOTO(s->hdr.stage, S_DONE, &s->w.phase, NULL);
            continue;
        }

        default:
            DCHECK(s->hdr.stage == S_DONE,
                   "a §4.2.4 machine resumed in a stage §4.2's pipe operations do not have");
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;   /* the answer, if this entry has one, is taken by js_pipe_fini */
        }
    }
}

/* ---- install ------------------------------------------------------------------------------------------- */

/* ONE STATE STRUCT, ONE STEP, FOURTEEN ENTRIES. `arg` is the operation, which is how a reaction says which of
   the four rules it is and how `pipeTo` and `pipeThrough` share one algorithm. `catches_abrupt` because the
   machine answers a throwing option getter itself — a rejection for pipeTo, a throw for pipeThrough — rather
   than letting the throw unwind it. */
#define PIPE_DEF(i) { sizeof(JSPipeState), js_pipe_step, js_pipe_fini, (i), \
                      .catches_abrupt = 1, .visit = js_pipe_visit, \
                      .algorithm = "Streams §4.2 pipeTo/pipeThrough, through §4.2.4 ReadableStreamPipeTo", \
                      .steps = PIPE_STEPS }
static const JSTrampStepDef js_pipe_defs[OP_N] = {
    PIPE_DEF(0),  PIPE_DEF(1),  PIPE_DEF(2),  PIPE_DEF(3),  PIPE_DEF(4),
    PIPE_DEF(5),  PIPE_DEF(6),  PIPE_DEF(7),  PIPE_DEF(8),  PIPE_DEF(9),
    PIPE_DEF(10), PIPE_DEF(11), PIPE_DEF(12), PIPE_DEF(13),
};
#undef PIPE_DEF

void pipe_init(JSContext *ctx)
{
    JSClassDef cd = { "PipeState", .finalizer = pipe_finalizer, .gc_mark = pipe_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    DCHECK(g_pipe_rt == NULL || g_pipe_rt == rt, "piping was installed into a second runtime");
    if (g_pipe_rt == rt) return;
    g_pipe_rt = rt;
    JS_NewClassID(rt, &g_pipe_class);
    JS_NewClass(rt, g_pipe_class, &cd);

    for (i = 0; i < OP_N; i++) {
        g_op_stepid[i] = JS_RegisterStepDef(rt, &js_pipe_defs[i]);
        CHECK(g_op_stepid[i] >= 0, "piping: no step id for a §4.2.4 operation");
    }
    g_pipe_to_stepid = g_op_stepid[OP_PIPE_TO];
    g_pipe_through_stepid = g_op_stepid[OP_PIPE_THROUGH];
}

void pipe_install(JSContext *ctx, JSValueConst stream_proto)
{
    DCHECK(g_pipe_to_stepid >= 0, "piping was installed before pipe_init declared its machines");
    /* §4.2's IDL lengths: `pipeThrough(transform, options)` is 1 required, `pipeTo(destination, options)` is
       also 1 — the options argument is optional in both. */
    idl_install_step_method(ctx, stream_proto, "pipeThrough", 1, g_pipe_through_stepid);
    idl_install_step_method(ctx, stream_proto, "pipeTo", 1, g_pipe_to_stepid);
}

void pipe_free(JSContext *ctx)
{
    int i;
    (void)ctx;
    if (!g_pipe_rt) return;
    g_pipe_rt = NULL;
    for (i = 0; i < OP_N; i++) g_op_stepid[i] = -1;
    g_pipe_to_stepid = g_pipe_through_stepid = -1;
}
