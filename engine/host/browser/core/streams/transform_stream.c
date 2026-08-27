/* TRANSFORMSTREAM — the Streams Standard §6.
 *
 * WHAT IT IS. A pair of streams and a rule joining them: everything written to the WRITABLE half is handed to
 * the page's `transform`, and whatever that enqueues comes out of the READABLE half. Neither half has an
 * underlying sink or source OBJECT at all — their algorithms are closures over the transform stream — which is
 * why §4 and §5 had to grow CreateReadableStream and CreateWritableStream before this file could exist.
 *
 * THE ONE PIECE OF STATE THAT BELONGS TO NEITHER HALF: BACKPRESSURE. The readable half filling up has to stop
 * the writable half accepting, and the two cannot see each other. §6 joins them with a PROMISE: while
 * `[[backpressure]]` is true a write waits on `[[backpressureChangePromise]]`, and the readable half's PULL is
 * what resolves it. So the readable side asking for more is literally the thing that lets the next write
 * through — no polling, no shared queue, and the whole coupling is one promise.
 *
 * THE FINISH PROMISE IS WHY CLOSE AND CANCEL ARE ONE ANSWER. §6.3's [[finishPromise]] is minted by whichever of
 * the three terminating paths arrives FIRST — the sink's close, the sink's abort, the source's cancel — and
 * every later one returns that same promise rather than starting over. Without it, a stream cancelled from the
 * readable side while its flush was still running would run the transformer's `cancel` a second time.
 *
 * WHY EVERY CONTROLLER CALL HERE IS A REQUEST. Erroring or closing a half means calling that half's
 * controller `error`/`close`, and those are STEP MACHINES — they settle promises by calling resolving
 * functions, which is running code. So §6.2's error operations cannot be plain C helpers however synchronous
 * the standard's prose looks; they are sub-sequences with their own state byte, driven the way §4.5's settle
 * loop and §4.2.4's shutdown are. A first draft of this file wrote them as helpers and could not compile,
 * which was the design telling the truth early. */
#include <stddef.h>

#include "check.h"
#include "solver/cow.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/streams/transform_stream.h"
#include "core/streams/readable_stream.h"
#include "core/streams/writable_stream.h"
#include "core/streams/stream_work.h"

/* ---- the records ----------------------------------------------------------------------------------------- */

/* §6.2's `Transformer`, in the order Web IDL converts it — LEXICOGRAPHIC, which a page pins by throwing from
   one member's getter and counting which others ran. */
enum { TR_CANCEL = 0, TR_FLUSH, TR_READABLE_TYPE, TR_START, TR_TRANSFORM, TR_WRITABLE_TYPE, TR_N };
static const char *const TR_NAMES[TR_N] = {
    "cancel", "flush", "readableType", "start", "transform", "writableType"
};

/* §6.2's [[backpressure]] is a TRI-STATE: it is UNSET until InitializeTransformStream's first SetBackpressure,
   whose own assert is that the value differs from the one already there. A boolean could not hold that. */
#define BP_UNSET 2

typedef struct {
    JSValue readable, writable;
    JSValue controller;
    JSValue bp_promise, bp_funcs[2];   /* [[backpressureChangePromise]] and the capability behind it */
    uint8_t backpressure;
} TsData;

typedef struct {
    JSValue stream;
    JSValue transformer;                       /* the receiver §6.3 invokes the algorithms on */
    JSValue transform_fn, flush_fn, cancel_fn;
    JSValue finish, finish_funcs[2];           /* [[finishPromise]]: the ONE answer every ending shares */
} TsCtrlData;

static JSClassID g_ts_class, g_tc_class;
static JSRuntime *g_ts_rt;

/* THE RECORDS TIME-TRAVEL, AND THE CAPTURE IS IN THE ACCESSOR — §4's comment on the same lines gives the whole
   reason. §6's exposure is the backpressure state: one flow writing while the stream is under backpressure
   installed a change promise every sibling then waited on, so a fork's arms blocked each other's writes. The
   offset lists are the same lists the finalizers free; the finalizers and gc_marks go through JS_GetOpaque. */
#define TS_OFF(T, f) (uint16_t)offsetof(T, f)
#define TS_NVAL(a)   (int)(sizeof(a) / sizeof((a)[0]))
static const uint16_t TSD_VALS[] = {
    TS_OFF(TsData, readable), TS_OFF(TsData, writable), TS_OFF(TsData, controller),
    TS_OFF(TsData, bp_promise), TS_OFF(TsData, bp_funcs[0]), TS_OFF(TsData, bp_funcs[1]),
};
static const CowRecord TSD_REC = { sizeof(TsData), TSD_VALS, TS_NVAL(TSD_VALS) };

static const uint16_t TCD_VALS[] = {
    TS_OFF(TsCtrlData, stream), TS_OFF(TsCtrlData, transformer), TS_OFF(TsCtrlData, transform_fn),
    TS_OFF(TsCtrlData, flush_fn), TS_OFF(TsCtrlData, cancel_fn), TS_OFF(TsCtrlData, finish),
    TS_OFF(TsCtrlData, finish_funcs[0]), TS_OFF(TsCtrlData, finish_funcs[1]),
};
static const CowRecord TCD_REC = { sizeof(TsCtrlData), TCD_VALS, TS_NVAL(TCD_VALS) };

static TsData *ts_of(JSValueConst v)
{
    TsData *t = JS_GetOpaque(v, g_ts_class);
    if (t) cow_capture_host_record(v, t, &TSD_REC);
    return t;
}
static TsCtrlData *tc_of(JSValueConst v)
{
    TsCtrlData *c = JS_GetOpaque(v, g_tc_class);
    if (c) cow_capture_host_record(v, c, &TCD_REC);
    return c;
}

bool transform_stream_is(JSValueConst v) { return g_ts_class != 0 && JS_GetOpaque(v, g_ts_class) != NULL; }

static void ts_finalizer(JSRuntime *rt, JSValue val)
{
    TsData *t = JS_GetOpaque(val, g_ts_class);
    int k;
    if (!t) return;
    JS_FreeValueRT(rt, t->readable);   JS_FreeValueRT(rt, t->writable);
    JS_FreeValueRT(rt, t->controller); JS_FreeValueRT(rt, t->bp_promise);
    for (k = 0; k < 2; k++) JS_FreeValueRT(rt, t->bp_funcs[k]);
    js_free_rt(rt, t);
}

static void ts_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    TsData *t = JS_GetOpaque(val, g_ts_class);
    int k;
    if (!t) return;
    JS_MarkValue(rt, t->readable, mark);   JS_MarkValue(rt, t->writable, mark);
    JS_MarkValue(rt, t->controller, mark); JS_MarkValue(rt, t->bp_promise, mark);
    for (k = 0; k < 2; k++) JS_MarkValue(rt, t->bp_funcs[k], mark);
}

static void tc_finalizer(JSRuntime *rt, JSValue val)
{
    TsCtrlData *c = JS_GetOpaque(val, g_tc_class);
    int k;
    if (!c) return;
    JS_FreeValueRT(rt, c->stream);        JS_FreeValueRT(rt, c->transformer);
    JS_FreeValueRT(rt, c->transform_fn);  JS_FreeValueRT(rt, c->flush_fn);
    JS_FreeValueRT(rt, c->cancel_fn);     JS_FreeValueRT(rt, c->finish);
    for (k = 0; k < 2; k++) JS_FreeValueRT(rt, c->finish_funcs[k]);
    js_free_rt(rt, c);
}

static void tc_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    TsCtrlData *c = JS_GetOpaque(val, g_tc_class);
    int k;
    if (!c) return;
    JS_MarkValue(rt, c->stream, mark);        JS_MarkValue(rt, c->transformer, mark);
    JS_MarkValue(rt, c->transform_fn, mark);  JS_MarkValue(rt, c->flush_fn, mark);
    JS_MarkValue(rt, c->cancel_fn, mark);     JS_MarkValue(rt, c->finish, mark);
    for (k = 0; k < 2; k++) JS_MarkValue(rt, c->finish_funcs[k], mark);
}

/* ---- the machine ----------------------------------------------------------------------------------------- */

enum {
    OP_CTOR = 0,
    /* the five algorithms the two halves call — each ANSWERS A PROMISE, as an underlying member must */
    OP_SINK_WRITE, OP_SINK_CLOSE, OP_SINK_ABORT, OP_SOURCE_PULL, OP_SOURCE_CANCEL,
    /* §6.3's members */
    OP_CTRL_ENQUEUE, OP_CTRL_ERROR, OP_CTRL_TERMINATE,
    /* the reactions */
    OP_BP_READY,                   /* backpressure cleared: the held write may transform now */
    OP_TRANSFORM_ERR,              /* PerformTransform's rejection: error the stream and re-throw */
    OP_FINISH_OK, OP_FINISH_ERR,   /* the flush's or the cancel's answer, settling [[finishPromise]] */
    OP_HELD_OK, OP_HELD_ERR,       /* a resumed write's transform answered: settle the capability it was handed */
    OP_SETUP,   /* Streams §9.3.1's "set up a TransformStream", for a spec layering its own interface over §6 */
    OP_N
};

/* WHERE THIS MACHINE RESTS, AS §6 NUMBERS IT. One machine walks §6.2's constructor, §6.4's five algorithms and
   §6.3's three members, so a stage names the OPERATION it is inside rather than this file's own count. */
#define TS_STAGES(X) \
    X(S_ENTRY, "Streams §6 (this invocation's entry: which of §6.2's constructor, §6.4's algorithms or §6.3's " \
               "members this is, and the stream and controller records it acts on)") \
    X(S_STRAT_READ, "Streams §6.2 new TransformStream steps 5-8 (reading the two QueuingStrategy " \
                    "dictionaries Web IDL has converted)") \
    X(S_HWM_W, "Streams §6.2 step 7 (ExtractHighWaterMark over writableStrategy — ToNumber on whatever the " \
               "page put there)") \
    X(S_HWM_R, "Streams §6.2 step 5 (ExtractHighWaterMark over readableStrategy)") \
    X(S_CTOR_PROTO, "Web IDL §3.7.1 (Get(newTarget, \"prototype\") — what makes " \
                    "`class T extends TransformStream {}` produce a T)") \
    X(S_TR_READ, "Streams §6.2 step 2 (converting transformer to a Transformer: one [[Get]] per member, in " \
                 "the order Web IDL §3.2.17 reads them)") \
    X(S_BUILD, "Streams §6.2 steps 3-11 (the two reserved types, InitializeTransformStream, and " \
               "SetUpTransformStreamDefaultControllerFromTransformer)") \
    X(S_BP_INIT, "Streams §6.4 InitializeTransformStream step 9 (the stream begins under backpressure, so " \
                 "TransformStreamSetBackpressure runs with true)") \
    X(S_START_W, "Streams §6.4 InitializeTransformStream step 5 (CreateWritableStream's start algorithm " \
                 "answers the shared startPromise)") \
    X(S_START_R, "Streams §6.4 InitializeTransformStream step 8 (CreateReadableStream's start algorithm " \
                 "answers the same startPromise)") \
    X(S_START, "Streams §6.2 step 12 (the transformer's `start`, invoked with the controller)") \
    X(S_START_SETTLE, "Streams §6.2 steps 12-13 (resolving startPromise with what `start` returned, or with " \
                      "undefined when the transformer declares none)") \
    X(S_WRITE_HOLD, "Streams §6.4 TransformStreamDefaultSinkWriteAlgorithm step 3.3 (the write is held on " \
                    "backpressureChangePromise until the readable half pulls)") \
    X(S_TRANSFORM, "Streams §6.4 TransformStreamDefaultControllerPerformTransform step 1 (the transform " \
                   "algorithm over the chunk)") \
    X(S_FLUSH, "Streams §6.4 TransformStreamDefaultSinkCloseAlgorithm step 5 (the flush algorithm)") \
    X(S_CANCEL, "Streams §6.4 TransformStreamDefaultSinkAbortAlgorithm step 5 / " \
                "TransformStreamDefaultSourceCancelAlgorithm step 5 (the cancel algorithm over the reason)") \
    X(S_FINISH_ERRORED, "Streams §6.4 SinkClose/SinkAbort/SourceCancel step 7 (what the flush or cancel " \
                        "answered decides which half is closed or errored)") \
    X(S_FINISH_SETTLE, "Streams §6.4 SinkClose/SinkAbort/SourceCancel step 7.1.2.2 (settling " \
                       "controller.[[finishPromise]])") \
    X(S_ENQUEUE, "Streams §6.3 TransformStreamDefaultControllerEnqueue steps 1-4 (CanCloseOrEnqueue, then " \
                 "ReadableStreamDefaultControllerEnqueue)") \
    X(S_ENQUEUE_BP, "Streams §6.3 TransformStreamDefaultControllerEnqueue steps 5-7 (the readable half's " \
                    "backpressure, set on the transform stream)") \
    X(S_ENQUEUE_DONE, "Streams §6.3 TransformStreamDefaultControllerEnqueue step 8 (the enqueue is complete)") \
    X(S_ERROR_STREAM, "Streams §6.3 TransformStreamDefaultControllerError step 1 (TransformStreamError with " \
                      "the reason)") \
    X(S_TERM_CLOSE, "Streams §6.3 TransformStreamDefaultControllerTerminate steps 2-3 " \
                    "(ReadableStreamDefaultControllerClose on the readable half)") \
    X(S_TERM_ERROR, "Streams §6.3 TransformStreamDefaultControllerTerminate steps 4-5 (the terminated " \
                    "TypeError, then TransformStreamErrorWritableAndUnblockWrite)") \
    X(S_PULL, "Streams §6.4 TransformStreamDefaultSourcePullAlgorithm steps 3-4 (clear the backpressure and " \
              "answer with the change promise)") \
    X(S_PROMISE_OF, "Streams §6 (PromiseResolve over what one of §6.4's algorithms returned, then its " \
                    "reaction — 27.5.1.3 step 2.f's `then` read is the page's)") \
    X(S_ERROR_SEQ, "Streams §6.4 TransformStreamError / TransformStreamErrorWritableAndUnblockWrite (the " \
                   "shared error sub-sequence, which half it errors being the only difference)") \
    X(S_SETBP, "Streams §6.4 TransformStreamSetBackpressure steps 2-4 (resolve the old change promise, then " \
               "a fresh one)") \
    X(S_SETTLE, "Streams §6 (settling the promise the step before this one named — the resolving function's " \
                "27.5.1.3 step 2.f `then` read is the page's code)") \
    X(S_RESULT, "Streams §6 (settling this entry's OWN capability, for the answers §6.4's algorithms " \
                "short-circuit with)") \
    X(S_DONE, "Streams §6 (the operation is complete; its promise, where it has one, is this machine's result)")
enum { TS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TS_STEPS[] = { TS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §6.2's TransformStreamError / TransformStreamErrorWritableAndUnblockWrite, as ONE sub-sequence over
   `w->settle`: which halves it errors is the only thing that differs between them. */
enum { E_IDLE = 0, E_READABLE, E_WRITABLE, E_UNBLOCK, E_DONE };
/* §6.2's TransformStreamSetBackpressure, nested INSIDE that one — which is what stream_work's second sequence
   byte exists for. */
enum { B_IDLE = 0, B_RESOLVE, B_FRESH };

typedef struct {
    JSStepHdr hdr;     /* FIRST — the trampoline driver writes the def and the operand bounds through it */
    /* THE HEADER IN FORCE. §6 has TWO entry shapes and one algorithm: the CONSTRUCTOR is an IDL declaration
       (Web IDL converts its two QueuingStrategy arguments before the body runs, and idl_step_constructor only
       accepts an id from that pool), while every other entry is a trampoline step def whose header is the one
       embedded above. Rather than two copies of the stage loop, each entry says which header its requests are
       parked on and the loop reads that. */
    JSStepHdr *h;
    StreamWork w;      /* the stage, the two nested sequences, whichever call is in flight, and its buffer */
    JSValue ts;        /* the TransformStream (owned) */
    JSValue ctrl;      /* its controller (owned) */
    JSValue tr[TR_N];  /* the Transformer's members, while the constructor reads them (owned) */
    /* The two QueuingStrategy dictionaries, read BEFORE the transformer because Web IDL converts arguments in
       order and only the transformer's conversion is in §6.2's BODY: [wHWM, wSize, rHWM, rSize]. */
    JSValue strat[4];
    JSValue promise, funcs[2];   /* this entry's answer */
    /* THE CANCEL REASON, HELD ACROSS THE ALGORITHM. §6.2's abort and cancel algorithms error the OTHER half
       with the reason THEY were given, not with whatever `cancel(reason)` fulfilled with — so the reason has
       to outlive the call, and it has to cross the reaction boundary too, because the fulfilment steps run in
       a fresh entry. Undefined on the flush path, which has no reason. */
    JSValue reason;
    JSValue proto;     /* `new.target.prototype`, while the constructor is between reading it and building */
    /* THE CAPABILITY A HELD WRITE WAS HANDED. It cannot live in the shared work record: PromiseResolve runs
       through that record's `func` and `chain`, and the held write's resolve/reject have to survive it. */
    JSValue held_funcs[2];
    uint8_t member;    /* which dictionary member the constructor's read loop is on — and nothing else */
    uint8_t reject;
    uint8_t next;      /* the stage a sub-sequence returns to */
    uint8_t bp_want;   /* the value the set-backpressure sub-sequence is installing */
    uint8_t err_both;  /* the error sub-sequence errors the READABLE half too */
    uint8_t held;      /* a write RESUMED from backpressure: it settles a capability it was handed */
    uint8_t side;      /* which ending a finish is finishing: 1 flush, 2 source cancel, 3 sink abort */
    uint8_t answer;    /* what this entry answers with: 0 its own, 1 the stream, 2 the change promise */
    uint8_t rethrow;   /* the transform's rejection, re-reported once the stream has been errored */
    /* A STAGE THAT HOLDS A CALL DECIDES ONCE, AT PHASE 0, AND REMEMBERS. Every predicate this file branches on
       is one the call it is about to make can CHANGE — enqueuing can error the readable half, closing makes
       CanCloseOrEnqueue false, minting the finish promise makes "already finishing" true — so re-reading it on
       the way back in picks a different branch and walks away from the request. ts_goto asserts that; this is
       where the answer is kept so the assert never has to fire. */
    uint8_t choice;
    /* THIS ENTRY IS §9.3.1's SET UP, not §6.2's constructor. The two build the same thing out of the same
       stages; what differs is where the algorithms come from (three arguments rather than a transformer's
       members), what `this` they are called with (undefined — there is no transformer object), and that the
       marks are the fixed ones §9.3.1 states rather than a page's strategies. */
    uint8_t setup;
    double  whwm, rhwm;   /* the two marks, once ToNumber has run on whatever the page put there */
} JSTsState;

static int g_op_stepid[OP_N];
static int g_ctor_stepid = -1;
/* §9.3's four operations, held as the FUNCTION OBJECTS this component installed — see transform_stream.h. */
/* §9.3's FOUR OPERATIONS AS FUNCTION OBJECTS, PER REALM — a function object carries the realm it was minted
   in, so one set held for the agent ran a child document's transform through the first document's realm. */
static int g_ts_fn_slot[TS_OP_N];

static void js_ts_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTsState *s = st;
    int k;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->ts);
    v->val(ctx, &s->ctrl);
    for (k = 0; k < TR_N; k++) v->val(ctx, &s->tr[k]);
    for (k = 0; k < 4; k++) v->val(ctx, &s->strat[k]);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->reason);
    v->val(ctx, &s->proto);
    for (k = 0; k < 2; k++) { v->val(ctx, &s->funcs[k]); v->val(ctx, &s->held_funcs[k]); }
}

static JSValue js_ts_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTsState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    (void)ctx;
    if (take_result) s->promise = JS_UNDEFINED;
    return r;
}

/* §6.3's ClearAlgorithms — the transformer's functions are dropped so what they close over can be collected,
   and so a second terminating path cannot run one of them again. */
static void ts_clear_algorithms(JSContext *ctx, TsCtrlData *c)
{
    JS_FreeValue(ctx, c->transform_fn);
    JS_FreeValue(ctx, c->flush_fn);
    JS_FreeValue(ctx, c->cancel_fn);
    c->transform_fn = c->flush_fn = c->cancel_fn = JS_UNDEFINED;
}

/* THE STAGE IS THE HEADER'S, AND THE TWO ENTRY SHAPES INDEX IT FROM DIFFERENT BASES. §6's constructor is an
   IDL declaration, so idl_args.c owns the first IDL_STEP_FIRST stages of that header and hands the rest to
   the body; every other entry is a trampoline def whose own stages start at 0. The BASE is not stored — the
   header in force says which shape this is, so the two numbers cannot drift apart. It is the same number the
   `steps` array is indexed from on each side, which is why one X-list serves both.
   It was a private byte on the shared work record: a resume point the driver's assert could not see, could not
   report at a park, and could not resolve back to a step in a later build. */
#define TS_BASE(s) ((s)->h == &(s)->hdr ? 0 : IDL_STEP_FIRST)
static unsigned ts_stage(const JSTsState *s) { return (unsigned)(s->h->stage - TS_BASE(s)); }
static void ts_set(JSTsState *s, int stage) { s->h->stage = (uint16_t)(stage + TS_BASE(s)); }

/* LEAVING A STAGE WITH A CALL IN FLIGHT IS THE BUG THIS ASSERT EXISTS FOR — the same rule §4.2.4 learned the
   hard way. A stage that holds a request must reach the same step_call_run to collect its result; deciding
   differently on the way back in abandons the call and leaves the phase byte set, and the NEXT request then
   reads that as a resume and answers without ever asking. */
static void ts_goto(JSTsState *s, int stage)
{
    DCHECK(s->w.phase == 0, "a §6 stage was left with a call still in flight");
    ts_set(s, stage);
}

/* This entry's own answer, settled at S_RESULT — the promise-returning algorithms and members end here. */
static int ts_short(JSContext *ctx, JSTsState *s, int reject, JSValue value)
{
    s->promise = JS_NewPromiseCapability(ctx, s->funcs);
    if (JS_IsException(s->promise)) { JS_FreeValue(ctx, value); return -1; }
    JS_FreeValue(ctx, s->w.value);
    s->w.value = value;
    s->reject = (uint8_t)reject;
    ts_goto(s, S_RESULT);
    return 0;
}

/* Enter the shared error sub-sequence, returning to `next` when it finishes. `err` is BORROWED. */
static void ts_error_enter(JSTsState *s, TsData *t, int both, JSValueConst err, int next, JSContext *ctx)
{
    JS_FreeValue(ctx, s->w.err);
    s->w.err = JS_DupValue(ctx, err);
    s->err_both = (uint8_t)both;
    s->w.settle = E_READABLE;
    s->next = (uint8_t)next;
    (void)t;
    ts_goto(s, S_ERROR_SEQ);
}

/* THE ONE STAGE LOOP. `entry_op` is the operation for a fresh entry and is ignored on a resume, where the
   stage the state is parked in says everything. */
static int ts_run(JSContext *ctx, JSTsState *s, JSStepHdr *hdr, int op, JSValue cb_result,
                  JSValue **out_cb, int *out_argc)
{
    TsData *t;
    TsCtrlData *c;
    JSValue out;
    int r;

    /* THE HEADER IN FORCE, BEFORE THE STAGE IS READ — because the stage IS this header's, and which base it is
       counted from is derived from which header this is. Recorded at the first entry only, it was still NULL
       when the very first ts_stage() ran. Every re-entry arrives through the same definition and therefore the
       same header, so assigning it on every entry says the same thing and cannot be reached too late. */
    s->h = hdr;

    if (ts_stage(s) == S_ENTRY) {
        stream_work_start(&s->w);
        s->ts = s->ctrl = s->promise = s->reason = s->proto = JS_UNDEFINED;
        s->funcs[0] = s->funcs[1] = JS_UNDEFINED;
        s->held_funcs[0] = s->held_funcs[1] = JS_UNDEFINED;
        { int k; for (k = 0; k < TR_N; k++) s->tr[k] = JS_UNDEFINED;
                 for (k = 0; k < 4; k++) s->strat[k] = JS_UNDEFINED; }
        s->member = s->reject = s->next = s->bp_want = s->err_both = 0;
        s->held = s->side = s->answer = s->rethrow = s->choice = s->setup = 0;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        if (op == OP_CTOR) {
            if (JS_IsUndefined(hdr->this_val)) {
                JS_ThrowTypeError(ctx, "constructor TransformStream requires 'new'");
                return JS_STEP_ABRUPT;
            }
            s->answer = 1;
            ts_set(s, S_STRAT_READ);
            goto run;
        }

        if (op == OP_SETUP) {
            /* §9.3.1: the marks and size algorithms are FIXED — a spec that sets up a transform stream does
               not get to pick them, and a page cannot reach them either. The three algorithms are the
               arguments; there is no `start`, so the shared start promise settles with undefined. */
            s->setup = 1;
            s->answer = 1;
            s->whwm = 1;
            s->rhwm = 0;
            s->tr[TR_TRANSFORM] = JS_DupValue(ctx, step_arg(hdr, 0));
            s->tr[TR_FLUSH]     = JS_DupValue(ctx, step_arg(hdr, 1));
            s->tr[TR_CANCEL]    = JS_DupValue(ctx, step_arg(hdr, 2));
            ts_set(s, S_BUILD);
            goto run;
        }

        if (op == OP_HELD_OK || op == OP_HELD_ERR) {
            /* A RESUMED WRITE'S ANSWER. It settles a capability it captured and touches neither half, so it
               resolves no stream and no controller — the two resolving functions ARE its whole world. */
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(hdr, 0));
            JS_FreeValue(ctx, s->w.func);
            s->w.func = JS_DupValue(ctx, JS_StepClosureData(hdr, op == OP_HELD_OK ? 0 : 1));
            s->next = S_DONE;
            ts_set(s, S_SETTLE);
            goto run;
        }

        /* Every other entry is either a §6.3 MEMBER (the receiver is the controller) or one of the five
           algorithms and their reactions (which captured the stream). Both resolve to the same pair. */
        if (op >= OP_CTRL_ENQUEUE && op <= OP_CTRL_TERMINATE) {
            if (!tc_of(hdr->this_val)) {
                JS_ThrowTypeError(ctx, "not a TransformStreamDefaultController");
                return JS_STEP_ABRUPT;
            }
            s->ctrl = JS_DupValue(ctx, hdr->this_val);
            s->ts = JS_DupValue(ctx, tc_of(s->ctrl)->stream);
        } else if (op == OP_FINISH_OK || op == OP_FINISH_ERR) {
            /* THE ACCESSOR IS CALLED ONCE, OUTSIDE THE ASSERT, and that is not a style preference: tc_of
               CAPTURES its record into the running flow's COW delta, and a DCHECK's condition is not evaluated
               in release — so an accessor reached only from inside one is a delta entry the release build never
               makes. It survived here only by the accident that the line after it called tc_of again, which is
               one edit away from not being true. The function's own `t`/`c` hold the answer rather than a local
               pair, because a shadowing declaration would leave the outer ones stale at the same time. */
            s->ctrl = JS_DupValue(ctx, JS_StepClosureData(hdr, 0));
            c = tc_of(s->ctrl);
            DCHECK(c != NULL, "a §6 finish reaction captured something that is not a controller");
            s->ts = JS_DupValue(ctx, c->stream);
        } else {
            s->ts = JS_DupValue(ctx, JS_StepClosureData(hdr, 0));
            t = ts_of(s->ts);
            DCHECK(t != NULL, "a §6 algorithm captured something that is not a TransformStream");
            s->ctrl = JS_DupValue(ctx, t->controller);
        }
        t = ts_of(s->ts);

        switch (op) {
        case OP_SINK_WRITE:
            /* §6.2's TransformStreamDefaultSinkWriteAlgorithm. */
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(hdr, 0));
            DCHECK(t->backpressure != BP_UNSET, "a §6 write reached a stream whose backpressure was never set");
            ts_set(s, t->backpressure ? S_WRITE_HOLD : S_TRANSFORM);
            break;

        case OP_BP_READY:
            /* §6.2's fulfilment steps for a held write. The chunk it was holding is the closure's second
               captured value, because the state that held it belonged to an entry that has already answered. */
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, JS_StepClosureData(hdr, 1));
            s->held_funcs[0] = JS_DupValue(ctx, JS_StepClosureData(hdr, 2));   /* the held write's resolve */
            s->held_funcs[1] = JS_DupValue(ctx, JS_StepClosureData(hdr, 3));   /* …and its reject */
            s->held = 1;   /* S_TRANSFORM settles the capability it was handed, not one of its own */
            ts_set(s, S_TRANSFORM);
            break;

        case OP_SINK_CLOSE:
            ts_set(s, S_FLUSH);
            break;
        case OP_SINK_ABORT: case OP_SOURCE_CANCEL:
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(hdr, 0));
            s->side = (uint8_t)(op == OP_SOURCE_CANCEL ? 2 : 3);
            ts_set(s, S_CANCEL);
            break;

        case OP_SOURCE_PULL:
            ts_set(s, S_PULL);
            break;

        case OP_CTRL_ENQUEUE:
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(hdr, 0));
            ts_set(s, S_ENQUEUE);
            break;
        case OP_CTRL_ERROR:
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(hdr, 0));
            ts_set(s, S_ERROR_STREAM);
            break;
        case OP_CTRL_TERMINATE:
            ts_set(s, S_TERM_CLOSE);
            break;

        case OP_TRANSFORM_ERR:
            /* §6.3's PerformTransform rejection steps: error the whole stream, then report the same reason. */
            ts_error_enter(s, t, 1, step_arg(hdr, 0), S_DONE, ctx);
            s->rethrow = 1;   /* …and report the same reason once the sequence finishes */
            break;

        default:
            DCHECK(op == OP_FINISH_OK || op == OP_FINISH_ERR,
                   "a §6 machine ran with an operation this component does not have");
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(hdr, 0));
            s->reject = (uint8_t)(op == OP_FINISH_ERR);
            s->side = (uint8_t)JS_VALUE_GET_INT(JS_StepClosureData(hdr, 1));
            JS_FreeValue(ctx, s->reason);
            s->reason = JS_DupValue(ctx, JS_StepClosureData(hdr, 2));
            ts_set(s, S_FINISH_ERRORED);
            break;
        }
        goto run;
    }

run:
    hdr = s->h;
    t = JS_IsUndefined(s->ts) ? NULL : ts_of(s->ts);
    c = JS_IsUndefined(s->ctrl) ? NULL : tc_of(s->ctrl);

    for (;;) {
        switch (ts_stage(s)) {

        /* ---- the two shared sub-sequences ---------------------------------------------------------------- */

        case S_SETBP: {
            /* §6.2's TransformStreamSetBackpressure. Resolving the OLD change promise is what releases a held
               write, so it is a call like any other; the fresh capability replaces it afterwards. */
            if (s->w.pull == B_IDLE) {
                DCHECK(t->backpressure != s->bp_want,
                       "§6.2 set the backpressure to the value it already had — the assert the standard states");
                s->w.pull = JS_IsUndefined(t->bp_funcs[0]) ? B_FRESH : B_RESOLVE;
            }
            if (s->w.pull == B_RESOLVE) {
                JSValueConst undef = JS_UNDEFINED;
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), t->bp_funcs[0], JS_UNDEFINED, 1, &undef,
                                  cb_result, &out, out_cb, out_argc);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                if (JS_IsException(out)) return JS_STEP_ABRUPT;
                JS_FreeValue(ctx, out);
                JS_FreeValue(ctx, t->bp_funcs[0]);
                JS_FreeValue(ctx, t->bp_funcs[1]);
                t->bp_funcs[0] = t->bp_funcs[1] = JS_UNDEFINED;
                s->w.pull = B_FRESH;
            }
            {
                JSValue funcs[2], p = JS_NewPromiseCapability(ctx, funcs);
                if (JS_IsException(p)) return JS_STEP_ABRUPT;
                JS_FreeValue(ctx, t->bp_promise);
                t->bp_promise = p;
                t->bp_funcs[0] = funcs[0];
                t->bp_funcs[1] = funcs[1];
                t->backpressure = s->bp_want;
            }
            s->w.pull = B_IDLE;
            ts_goto(s, s->next);
            continue;
        }

        case S_ERROR_SEQ: {
            /* §6.2's TransformStreamError when `err_both`, and ErrorWritableAndUnblockWrite when not — the
               two differ by one step, so they are one sequence with a flag rather than two that drift. */
            if (s->w.settle == E_READABLE) {
                if (s->err_both) {
                    JSValueConst arg = s->w.err;
                    JSValue op = readable_stream_ctrl_op(ctx, RS_CTRL_ERROR);
                    r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                      readable_stream_controller(t->readable), 1, &arg, cb_result, &out,
                                      out_cb, out_argc);
                    JS_FreeValue(ctx, op);
                    if (r > 0) return r;
                    cb_result = JS_UNDEFINED;
                    if (JS_IsException(out)) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, out);
                }
                if (c) ts_clear_algorithms(ctx, c);
                s->w.settle = E_WRITABLE;
            }
            if (s->w.settle == E_WRITABLE) {
                /* §5.4's `error` IS ErrorIfNeeded: it already returns without doing anything unless the
                   stream is still writable, so there is no second predicate here. */
                JSValueConst arg = s->w.err;
                JSValue op = writable_stream_op(ctx, WS_OP_CTRL_ERROR);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                  writable_stream_controller(t->writable), 1, &arg, cb_result, &out,
                                  out_cb, out_argc);
                JS_FreeValue(ctx, op);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                if (JS_IsException(out)) return JS_STEP_ABRUPT;
                JS_FreeValue(ctx, out);
                s->w.settle = E_UNBLOCK;
            }
            /* §6.2's UnblockWrite: a write held on backpressure is let go, so it FAILS on the now-errored
               stream rather than waiting on one nobody will read. */
            s->w.settle = E_IDLE;
            if (t->backpressure == 1) {
                s->bp_want = 0;
                ts_goto(s, S_SETBP);
                continue;
            }
            ts_goto(s, s->next);
            continue;
        }

        /* ---- the constructor ----------------------------------------------------------------------------- */

        case S_STRAT_READ: {
            /* §6.2's IDL converts arguments IN ORDER, and only the transformer's conversion is in the body —
               so both QueuingStrategy dictionaries are read BEFORE any transformer member, writable one
               first, `highWaterMark` before `size` within each. A page pins that by throwing from one getter
               and counting which others ran. */
            static const char *const SN[4] = { "highWaterMark", "size", "highWaterMark", "size" };
            while (s->member < 4) {
                JSValueConst strategy = step_arg(hdr, s->member < 2 ? 1 : 2);
                JSAtom a;
                if (!JS_IsObject(strategy)) { s->member++; continue; }
                a = JS_NewAtom(ctx, SN[s->member]);
                r = step_getprop_run(ctx, hdr, strategy, a, cb_result, &s->strat[s->member], out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
                s->member++;
            }
            if (stream_callback_member(ctx, s->strat[1], "queuing strategy", "size") < 0 ||
                stream_callback_member(ctx, s->strat[3], "queuing strategy", "size") < 0)
                return JS_STEP_ABRUPT;
            s->member = 0;
            s->whwm = 1;   /* §6.2's defaults: 1 for the writable half, 0 for the readable one */
            s->rhwm = 0;
            ts_goto(s, S_HWM_W);
            continue;
        }

        case S_HWM_W: case S_HWM_R: {
            /* §6.2's ExtractHighWaterMark is `? ToNumber(...)`, and ToNumber on an object runs the page's
               `valueOf` — so it is a request, not a JS_ToFloat64 from C. Two marks, two stages, because the
               coercion sub-sequence keeps one phase on the header and a second would overwrite it. */
            int w = (ts_stage(s) == S_HWM_W);
            JSValueConst v = s->strat[w ? 0 : 2];
            if (!JS_IsUndefined(v)) {
                r = step_todouble_run(ctx, hdr, v, cb_result, w ? &s->whwm : &s->rhwm, out_cb, out_argc);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
            }
            ts_goto(s, w ? S_HWM_R : S_CTOR_PROTO);
            continue;
        }

        case S_CTOR_PROTO: {
            /* Web IDL §3.7.1: the object is created from `? Get(newTarget, "prototype")` when that is an
               Object, and from the interface prototype object otherwise — which is the whole of what makes
               `class S extends TransformStream {}` produce an S. It sits HERE because Web IDL converts the
               arguments first and only then creates the object, so both strategies have been read (and their
               marks coerced) while none of the transformer's members has. The read is a REQUEST: new.target
               is whatever constructor a `Reflect.construct` names, so `prototype` can be the page's accessor. */
            JSAtom a = JS_NewAtom(ctx, "prototype");
            r = step_getprop_run(ctx, hdr, hdr->this_val, a, cb_result, &s->proto, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            ts_goto(s, S_TR_READ);
            continue;
        }

        case S_TR_READ: {
            JSValueConst transformer = step_arg(hdr, 0);
            while (s->member < TR_N && JS_IsObject(transformer)) {
                JSAtom a = JS_NewAtom(ctx, TR_NAMES[s->member]);
                r = step_getprop_run(ctx, hdr, transformer, a, cb_result, &s->tr[s->member], out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
                s->member++;
            }
            /* §6.2 steps 3 and 4: `readableType` and `writableType` are RESERVED, and ANY value for either is
               a RangeError rather than a silent fall-through to the default controller. */
            if (!JS_IsUndefined(s->tr[TR_READABLE_TYPE])) {
                JS_ThrowRangeError(ctx, "a transformer may not declare a `readableType`");
                return JS_STEP_ABRUPT;
            }
            if (!JS_IsUndefined(s->tr[TR_WRITABLE_TYPE])) {
                JS_ThrowRangeError(ctx, "a transformer may not declare a `writableType`");
                return JS_STEP_ABRUPT;
            }
            if (stream_callback_member(ctx, s->tr[TR_CANCEL], "transformer", "cancel") < 0 ||
                stream_callback_member(ctx, s->tr[TR_FLUSH], "transformer", "flush") < 0 ||
                stream_callback_member(ctx, s->tr[TR_START], "transformer", "start") < 0 ||
                stream_callback_member(ctx, s->tr[TR_TRANSFORM], "transformer", "transform") < 0)
                return JS_STEP_ABRUPT;
            s->member = 0;
            ts_goto(s, S_BUILD);
            continue;
        }

        case S_BUILD: {
            double rhwm = s->rhwm, whwm = s->whwm;
            TsData *td;
            TsCtrlData *cd;
            int k;

            /* §6.2 steps 5-8: ExtractHighWaterMark with a default of 0 for the readable half and 1 for the
               writable one — the asymmetry is what makes a fresh transform stream accept one write and hold
               the next until something reads. */
            /* §6.2's own check, not the type's: `unrestricted double` accepts NaN and the STREAM rejects it. */
            if (rhwm != rhwm || rhwm < 0 || whwm != whwm || whwm < 0) {
                JS_ThrowRangeError(ctx, "a queuing strategy's highWaterMark must not be negative or NaN");
                return JS_STEP_ABRUPT;
            }

            /* Both records are COMPLETE and attached before the first step that can fail. */
            JSValue tsp = JS_GetClassProto(ctx, g_ts_class);
            DCHECK(!JS_IsNull(tsp), "a TransformStream was minted in a realm that never ran its install");
            s->ts = JS_NewObjectProtoClass(ctx, JS_IsObject(s->proto) ? (JSValueConst)s->proto : tsp,
                                           g_ts_class);
            if (JS_IsException(s->ts)) return JS_STEP_ABRUPT;
            td = js_mallocz(ctx, sizeof *td);
            if (!td) return JS_STEP_ABRUPT;
            td->readable = td->writable = td->controller = td->bp_promise = JS_UNDEFINED;
            td->bp_funcs[0] = td->bp_funcs[1] = JS_UNDEFINED;
            td->backpressure = BP_UNSET;
            JS_SetOpaque(s->ts, td);

            JS_FreeValue(ctx, tsp);
            {
                JSValue tcp = JS_GetClassProto(ctx, g_tc_class);
                DCHECK(!JS_IsNull(tcp), "a §6.3 controller was minted in a realm with no install");
                s->ctrl = JS_NewObjectProtoClass(ctx, tcp, g_tc_class);
                JS_FreeValue(ctx, tcp);
            }
            if (JS_IsException(s->ctrl)) return JS_STEP_ABRUPT;
            cd = js_mallocz(ctx, sizeof *cd);
            if (!cd) return JS_STEP_ABRUPT;
            cd->stream = cd->transformer = JS_UNDEFINED;
            cd->transform_fn = cd->flush_fn = cd->cancel_fn = JS_UNDEFINED;
            cd->finish = cd->finish_funcs[0] = cd->finish_funcs[1] = JS_UNDEFINED;
            JS_SetOpaque(s->ctrl, cd);
            cd->stream = JS_DupValue(ctx, s->ts);
            td->controller = JS_DupValue(ctx, s->ctrl);
            /* §9.3.1's algorithms are the SPEC's, not a transformer's methods, so they are called with
               `this` = undefined; §6.2's are called with the transformer as the callback this value. */
            cd->transformer = s->setup ? JS_UNDEFINED : JS_DupValue(ctx, step_arg(hdr, 0));
            cd->transform_fn = s->tr[TR_TRANSFORM]; s->tr[TR_TRANSFORM] = JS_UNDEFINED;
            cd->flush_fn = s->tr[TR_FLUSH];         s->tr[TR_FLUSH] = JS_UNDEFINED;
            cd->cancel_fn = s->tr[TR_CANCEL];       s->tr[TR_CANCEL] = JS_UNDEFINED;

            /* THE TWO HALVES, each built from CLOSURES over this stream — the reason §4 and §5 grew their
               create operations. Each half's `size` is the page's, from the strategy it belongs to. */
            {
                JSValue fns[5];
                static const int OPS[5] = { OP_SINK_WRITE, OP_SINK_CLOSE, OP_SINK_ABORT,
                                            OP_SOURCE_PULL, OP_SOURCE_CANCEL };
                for (k = 0; k < 5; k++) {
                    fns[k] = JS_NewStepClosure(ctx, g_op_stepid[OPS[k]], 1, 1, (JSValueConst *)&s->ts);
                    if (JS_IsException(fns[k])) {
                        while (--k >= 0) JS_FreeValue(ctx, fns[k]);
                        return JS_STEP_ABRUPT;
                    }
                }
                td->writable = writable_stream_create(ctx, fns[0], fns[1], fns[2], whwm, s->strat[1]);
                td->readable = readable_stream_create(ctx, fns[3], fns[4], rhwm, s->strat[3]);
                for (k = 0; k < 5; k++) JS_FreeValue(ctx, fns[k]);
            }
            if (JS_IsException(td->writable) || JS_IsException(td->readable)) return JS_STEP_ABRUPT;

            t = td; c = cd;
            s->bp_want = 1;
            s->next = S_BP_INIT;
            ts_goto(s, S_SETBP);
            continue;
        }

        case S_BP_INIT:
            /* §6.2 steps 9 and 12: ONE start promise, given to BOTH halves and resolved with what the
               transformer's `start` returned. Handing the same promise to both is what makes the two halves
               start together, and it is why CreateReadable/WritableStream do not run a start of their own. */
            s->promise = JS_NewPromiseCapability(ctx, s->funcs);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, s->promise);
            ts_goto(s, S_START_W);
            continue;

        case S_START_W: case S_START_R: {
            /* EACH HALF GETS ITS OWN PROMISE, and that is not a detail — it is two ticks of the event loop.
               SetUp{Readable,Writable}StreamDefaultController each perform "Let startPromise be A PROMISE
               RESOLVED WITH startResult", and Web IDL's "a promise resolved with" mints a NEW promise and
               RESOLVES it with the value. Resolving with a thenable adopts it: one job to read `then` and call
               it, another for the reaction it registers. So a half does not start when the shared promise
               settles — it starts two jobs later, and anything queued in between runs first.
               `readable.cancel()` immediately followed by `controller.terminate()` is exactly that: the cancel's
               fulfilment steps land while the writable half is still "erroring" rather than "errored", so
               [[finishPromise]] RESOLVES and the cancel does not reject. Handing both halves the shared promise
               object skips the adoption and reverses that. The writable half is first because
               InitializeTransformStream creates it first, and its two jobs are queued in that order. */
            int is_w = (ts_stage(s) == S_START_W);
            r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            if ((is_w ? writable_stream_start(ctx, t->writable, s->w.func)
                      : readable_stream_start(ctx, t->readable, s->w.func)) < 0)
                return JS_STEP_ABRUPT;
            if (is_w) { ts_goto(s, S_START_R); continue; }
            /* BOTH HALVES HAVE ADOPTED IT, so the shared promise stops being this machine's argument. Left in
               place it became the value S_START_SETTLE resolves the shared capability WITH — a promise resolved
               with itself — on every stream whose transformer has no `start` to overwrite it. */
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_UNDEFINED;
            JS_FreeValue(ctx, s->promise);
            s->promise = JS_UNDEFINED;
            JS_FreeValue(ctx, s->w.func);
            s->w.func = s->tr[TR_START];
            s->tr[TR_START] = JS_UNDEFINED;
            ts_goto(s, JS_IsFunction(ctx, s->w.func) ? S_START : S_START_SETTLE);
            continue;
        }

        case S_START: {
            JSValueConst arg = s->ctrl;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, c->transformer, 1, &arg,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            /* §6.2 invokes `start` directly rather than through PromiseCall, so a throw propagates out of the
               constructor — which is why this machine does not declare catches_abrupt. */
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->w.value);
            s->w.value = out;
            ts_goto(s, S_START_SETTLE);
            continue;
        }

        case S_START_SETTLE:
            /* Resolve the shared start promise with whatever `start` answered — a thenable included, which is
               why it goes through the capability's resolve function rather than being stored. */
            JS_FreeValue(ctx, s->w.func);
            s->w.func = JS_DupValue(ctx, s->funcs[0]);
            s->next = S_DONE;
            ts_goto(s, S_SETTLE);
            continue;

        /* ---- the writable half's algorithms --------------------------------------------------------------- */

        case S_WRITE_HOLD: {
            /* The stream is applying backpressure, so this write WAITS. Its own capability is what the
               resumed transform settles, and both travel to the reaction as captured values — the state that
               is holding them belongs to an entry that is about to answer and be torn down. */
            JSValue data[4], fn;
            s->promise = JS_NewPromiseCapability(ctx, s->funcs);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            data[0] = s->ts;
            data[1] = s->w.value;
            data[2] = s->funcs[0];
            data[3] = s->funcs[1];
            fn = JS_NewStepClosure(ctx, g_op_stepid[OP_BP_READY], 1, 4, (JSValueConst *)data);
            if (JS_IsException(fn)) return JS_STEP_ABRUPT;
            {
                /* Both sides run the same continuation: the change promise is the ENGINE's and is only ever
                   resolved, so a rejection arm exists to be impossible rather than to differ. */
                JSValue cap = JS_PerformPromiseThen(ctx, t->bp_promise, fn, fn);
                JS_FreeValue(ctx, fn);
                if (JS_IsException(cap)) return JS_STEP_ABRUPT;
                JS_MarkPromiseHandled(ctx, cap);
                JS_FreeValue(ctx, cap);
            }
            ts_goto(s, S_DONE);
            continue;
        }

        case S_TRANSFORM: {
            /* §6.3's PerformTransform. A transformer with no `transform` enqueues the chunk unchanged, which
               is what makes `new TransformStream()` an identity transform. */
            if (s->w.phase == 0 && s->held) {
                /* §6.2's fulfilment steps: the writable half may have begun erroring while this write waited,
                   and then it fails rather than reaching the transformer. */
                WritableStreamState ws = WS_WRITABLE;
                writable_stream_query(t->writable, &ws, NULL, NULL);
                if (ws != WS_WRITABLE) {
                    JS_FreeValue(ctx, s->w.value);
                    s->w.value = writable_stream_stored_error(ctx, t->writable);
                    s->reject = 1;
                    JS_FreeValue(ctx, s->w.func);
                    s->w.func = JS_DupValue(ctx, s->held_funcs[1]);
                    s->held = 0;
                    s->next = S_DONE;
                    ts_goto(s, S_SETTLE);
                    continue;
                }
            }
            if (s->w.phase == 0 && (!c || JS_IsUndefined(c->transform_fn))) {
                ts_goto(s, S_ENQUEUE);
                continue;
            }
            {
                JSValueConst args[2];
                args[0] = s->w.value;
                args[1] = s->ctrl;
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), c->transform_fn, c->transformer, 2, args,
                                  cb_result, &out, out_cb, out_argc);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
            }
            /* §6.3 builds the transform algorithm with PromiseCall, so a THROW becomes a rejected promise and
               takes the same path a rejecting one does — which here is the OP_TRANSFORM_ERR reaction. */
            if (JS_IsException(out)) {
                /* PromiseCall would have made this a rejected promise and the reaction below would have
                   errored the stream; reaching that conclusion here is the same steps without minting a
                   promise for the engine to immediately consume. */
                JSValue e = JS_GetException(ctx);
                ts_error_enter(s, t, 1, e, s->held ? S_ENQUEUE_DONE : S_DONE, ctx);
                JS_FreeValue(ctx, e);
                s->reject = 1;
                if (!s->held) s->rethrow = 1;
                continue;
            }
            /* §6.3's transform algorithm is a PromiseCall, so what it RETURNED — a value, a thenable or a
               promise — becomes a promise before anything reacts to it. That is a PromiseResolve, and a
               PromiseResolve runs the page's `then` getter, so it is a request like every other. */
            JS_FreeValue(ctx, s->w.value);
            s->w.value = out;
            s->choice = 1;
            ts_goto(s, S_PROMISE_OF);
            continue;
        }

        case S_PROMISE_OF: {
            r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            if (s->choice == 1) {
                /* the TRANSFORM's answer: a rejection errors the stream, through OP_TRANSFORM_ERR. */
                JSValue cap = stream_react_cap(ctx, s->w.func, -1, g_op_stepid[OP_TRANSFORM_ERR],
                                               (JSValueConst *)&s->ts, 1);
                if (JS_IsException(cap)) return JS_STEP_ABRUPT;
                if (s->held) {
                    /* A HELD write settles the capability it was HANDED, not one of its own. */
                    r = stream_react(ctx, cap, g_op_stepid[OP_HELD_OK], g_op_stepid[OP_HELD_ERR],
                                     (JSValueConst *)s->held_funcs, 2);
                    JS_FreeValue(ctx, cap);
                    if (r < 0) return JS_STEP_ABRUPT;
                    s->held = 0;
                } else {
                    JS_FreeValue(ctx, s->promise);
                    s->promise = cap;
                }
            } else {
                /* a FLUSH or a CANCEL: the reaction settles [[finishPromise]] and knows which ending it was —
                   and, for the two cancels, what reason it is to error the other half with. */
                JSValueConst data[3];
                JSValue which = JS_NewInt32(ctx, s->side);
                JSValue cap;
                data[0] = s->ctrl;
                data[1] = which;
                data[2] = s->reason;
                cap = stream_react_cap(ctx, s->w.func, g_op_stepid[OP_FINISH_OK],
                                       g_op_stepid[OP_FINISH_ERR], data, 3);
                JS_FreeValue(ctx, which);
                if (JS_IsException(cap)) return JS_STEP_ABRUPT;
                JS_MarkPromiseHandled(ctx, cap);
                JS_FreeValue(ctx, cap);
            }
            s->choice = 0;
            ts_goto(s, S_DONE);
            continue;
        }

        case S_ENQUEUE: {
            /* §6.3's Enqueue — reached both by the controller's member and by an absent `transform`. */
            JSValueConst rc = readable_stream_controller(t->readable);
            if (s->w.phase == 0 && !readable_ctrl_can_close_or_enqueue(ctx, rc)) {
                JS_ThrowTypeError(ctx, "the transform stream's readable side can no longer be enqueued to");
                if (op == OP_CTRL_ENQUEUE) return JS_STEP_ABRUPT;
                if (ts_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            {
                JSValueConst arg = s->w.value;
                JSValue op = readable_stream_ctrl_op(ctx, RS_CTRL_ENQUEUE);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                  rc, 1, &arg, cb_result, &out, out_cb, out_argc);
                JS_FreeValue(ctx, op);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
            }
            if (JS_IsException(out)) {
                /* §6.3 step 4: a failed enqueue errors the WRITABLE half with the enqueue's own reason and
                   then reports the READABLE half's stored error, which need not be the same value. */
                JSValue e = JS_GetException(ctx);
                ts_error_enter(s, t, 0, e, S_ENQUEUE_DONE, ctx);
                JS_FreeValue(ctx, e);
                s->reject = 1;
                continue;
            }
            JS_FreeValue(ctx, out);
            ts_goto(s, S_ENQUEUE_BP);
            continue;
        }

        case S_ENQUEUE_BP:
            /* §6.3 steps 5 and 6: the readable half filling up is what applies backpressure to the writable
               one. This is the ONLY place backpressure is turned on after start. */
            if (readable_ctrl_has_backpressure(ctx, readable_stream_controller(t->readable)) &&
                t->backpressure != 1) {
                s->bp_want = 1;
                s->next = S_ENQUEUE_DONE;
                ts_goto(s, S_SETBP);
                continue;
            }
            ts_goto(s, S_ENQUEUE_DONE);
            continue;

        case S_ENQUEUE_DONE:
            if (s->reject) {
                /* the failed-enqueue path: the readable half's stored error is what everyone hears */
                JSValue e = readable_stream_stored_error(ctx, t->readable);
                if (op == OP_CTRL_ENQUEUE) { JS_Throw(ctx, e); return JS_STEP_ABRUPT; }
                if (s->held) {
                    JS_FreeValue(ctx, s->w.value);
                    s->w.value = e;
                    JS_FreeValue(ctx, s->w.func);
                    s->w.func = JS_DupValue(ctx, s->held_funcs[1]);
                    s->held = 0;
                    s->next = S_DONE;
                    ts_goto(s, S_SETTLE);
                    continue;
                }
                if (ts_short(ctx, s, 1, e) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            if (op == OP_CTRL_ENQUEUE) { ts_goto(s, S_DONE); continue; }
            if (s->held) {
                /* a HELD write: settle the capability it was handed */
                JS_FreeValue(ctx, s->w.value);
                s->w.value = JS_UNDEFINED;
                JS_FreeValue(ctx, s->w.func);
                s->w.func = JS_DupValue(ctx, s->held_funcs[0]);
                s->held = 0;
                s->next = S_DONE;
                ts_goto(s, S_SETTLE);
                continue;
            }
            if (ts_short(ctx, s, 0, JS_UNDEFINED) < 0) return JS_STEP_ABRUPT;
            continue;

        /* ---- §6.3's other members ------------------------------------------------------------------------ */

        case S_ERROR_STREAM:
            ts_error_enter(s, t, 1, s->w.value, S_DONE, ctx);
            continue;

        case S_TERM_CLOSE: {
            /* §6.3's Terminate: the readable half CLOSES and the writable half is errored, because there is
               nowhere left for what it would accept to go. */
            JSValueConst rc = readable_stream_controller(t->readable);
            if (s->w.phase == 0) s->choice = (uint8_t)readable_ctrl_can_close_or_enqueue(ctx, rc);
            if (s->choice) {
                JSValue op = readable_stream_ctrl_op(ctx, RS_CTRL_CLOSE);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                  rc, 0, NULL, cb_result, &out, out_cb, out_argc);
                JS_FreeValue(ctx, op);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                if (JS_IsException(out)) return JS_STEP_ABRUPT;
                JS_FreeValue(ctx, out);
            }
            ts_goto(s, S_TERM_ERROR);
            continue;
        }

        case S_TERM_ERROR: {
            JSValue e;
            JS_ThrowTypeError(ctx, "the transform stream has been terminated");
            e = JS_GetException(ctx);
            ts_error_enter(s, t, 0, e, S_DONE, ctx);
            JS_FreeValue(ctx, e);
            continue;
        }

        /* ---- the readable half's algorithms --------------------------------------------------------------- */

        case S_PULL:
            /* §6.2's TransformStreamDefaultSourcePullAlgorithm: the readable half asking for more is exactly
               what clears the backpressure, and its ANSWER is the promise the next write will wait on. */
            DCHECK(t->backpressure == 1, "§6.2 pulled a transform stream that was not applying backpressure");
            s->bp_want = 0;
            s->next = S_DONE;
            ts_goto(s, S_SETBP);
            /* the answer is the FRESH change promise, so it is taken after the sub-sequence has made one */
            s->answer = 2;
            continue;

        /* ---- the three terminating paths ----------------------------------------------------------------- */

        case S_FLUSH: case S_CANCEL: {
            int is_flush = (ts_stage(s) == S_FLUSH);
            JSValueConst arg = is_flush ? (JSValueConst)s->ctrl : (JSValueConst)s->w.value;

            if (s->w.phase == 0) {
                /* §6.2: whichever path arrives first mints [[finishPromise]]; every later one returns it. */
                if (!JS_IsUndefined(c->finish)) {
                    JS_FreeValue(ctx, s->promise);
                    s->promise = JS_DupValue(ctx, c->finish);
                    ts_goto(s, S_DONE);
                    continue;
                }
                c->finish = JS_NewPromiseCapability(ctx, c->finish_funcs);
                if (JS_IsException(c->finish)) { c->finish = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                JS_FreeValue(ctx, s->promise);
                s->promise = JS_DupValue(ctx, c->finish);
                /* THE ALGORITHM IS TAKEN NOW AND HELD. `flush(controller)` may call `controller.terminate()`,
                   and terminate CLEARS the algorithms — so a resume that re-read `c->flush_fn` would find it
                   gone, take the no-algorithm branch and walk away from the call it had already made. That is
                   a real test (`terminate() inside flush()`), and it is the same shape as every other decision
                   here that the call itself changes. */
                JS_FreeValue(ctx, s->w.func);
                s->w.func = JS_DupValue(ctx, is_flush ? c->flush_fn : c->cancel_fn);
                s->choice = (uint8_t)JS_IsFunction(ctx, s->w.func);
                /* AND SO IS THE REASON. §6.2's fulfilment steps error the other half with the reason this
                   algorithm was GIVEN, which the algorithm's own answer is about to overwrite. */
                JS_FreeValue(ctx, s->reason);
                s->reason = is_flush ? JS_UNDEFINED : JS_DupValue(ctx, s->w.value);
            }
            if (!s->choice) {
                out = JS_UNDEFINED;
            } else {
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, c->transformer, 1, &arg,
                                  cb_result, &out, out_cb, out_argc);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
            }
            /* §6.2 clears the algorithms BEFORE the answer is awaited, so nothing can run one again. */
            ts_clear_algorithms(ctx, c);
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, s->w.value);
                s->w.value = JS_GetException(ctx);
                s->reject = 1;
                s->side = (uint8_t)(is_flush ? 1 : s->side);
                ts_goto(s, S_FINISH_ERRORED);
                continue;
            }
            /* PromiseCall again: what the algorithm answered becomes a promise first. */
            s->side = (uint8_t)(is_flush ? 1 : s->side);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = out;
            s->choice = 2;
            ts_goto(s, S_PROMISE_OF);
            continue;
        }

        case S_FINISH_ERRORED: {
            /* §6.2's fulfilment and rejection steps for all three endings. They are ONE shape with two things
               that vary by ending, and getting either wrong is a real test:
                 - WHICH HALF this ending is about. A flush and a sink abort are about the READABLE half; a
                   SOURCE CANCEL is about the WRITABLE one — its steps consult writable.[[state]] and error the
                   writable controller. Consulting the readable half there reports the wrong stored error and
                   leaves the writable half live.
                 - WHAT VALUE errors it. On a REJECTION it is the rejection reason. On a FULFILMENT it is the
                   reason the CANCEL WAS GIVEN — `cancel(reason)` fulfilling with undefined must still error
                   the other half with `reason`, which is why the reason is held across the call.
               A clean FLUSH is the one ending that errors nothing: the readable half closes. */
            int is_source = (s->side == 2);

            if (s->w.phase == 0) {
                bool errored;
                if (is_source) {
                    WritableStreamState ws = WS_WRITABLE;
                    writable_stream_query(t->writable, &ws, NULL, NULL);
                    errored = (ws == WS_ERRORED);
                } else {
                    ReadableStreamState rs = RS_READABLE;
                    readable_stream_query(t->readable, &rs, NULL);
                    errored = (rs == RS_ERRORED);
                }
                /* 1 error the half with w.value, 2 report the half's own stored error, 3 close the readable */
                if (s->reject) {
                    s->choice = 1;
                } else if (errored) {
                    s->choice = 2;
                } else if (s->side == 1) {
                    s->choice = 3;
                } else {
                    /* the cancel's fulfilment: the REASON is what errors the other half, and this entry still
                       resolves [[finishPromise]] with undefined afterwards. */
                    JS_FreeValue(ctx, s->w.value);
                    s->w.value = JS_DupValue(ctx, s->reason);
                    s->choice = 1;
                }
            }
            if (s->choice == 1) {
                if (is_source) {
                    /* §6.2's ErrorWritableAndUnblockWrite: ErrorIfNeeded on the writable half, then a write
                       held on backpressure is let go so it fails rather than waiting forever. */
                    ts_error_enter(s, t, 0, s->w.value, S_FINISH_SETTLE, ctx);
                } else {
                    JSValueConst arg = s->w.value;
                    JSValue op = readable_stream_ctrl_op(ctx, RS_CTRL_ERROR);
                    r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                      readable_stream_controller(t->readable), 1, &arg, cb_result, &out,
                                      out_cb, out_argc);
                    JS_FreeValue(ctx, op);
                    if (r > 0) return r;
                    cb_result = JS_UNDEFINED;
                    if (JS_IsException(out)) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, out);
                    ts_goto(s, S_FINISH_SETTLE);
                }
                if (!s->reject) {
                    /* it errored the half with the reason; the promise itself resolves with undefined */
                    JS_FreeValue(ctx, s->w.value);
                    s->w.value = JS_UNDEFINED;
                }
                continue;
            }
            if (s->choice == 2) {
                JS_FreeValue(ctx, s->w.value);
                s->w.value = is_source ? writable_stream_stored_error(ctx, t->writable)
                                       : readable_stream_stored_error(ctx, t->readable);
                s->reject = 1;
                ts_goto(s, S_FINISH_SETTLE);
                continue;
            }
            /* the FLUSH finished cleanly: the readable half closes */
            DCHECK(s->choice == 3, "a §6 finish took a branch this component does not have");
            {
                JSValue op = readable_stream_ctrl_op(ctx, RS_CTRL_CLOSE);
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), op,
                                  readable_stream_controller(t->readable), 0, NULL, cb_result, &out,
                                  out_cb, out_argc);
                JS_FreeValue(ctx, op);
            }
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_UNDEFINED;
            ts_goto(s, S_FINISH_SETTLE);
            continue;
        }

        case S_FINISH_SETTLE:
            JS_FreeValue(ctx, s->w.func);
            s->w.func = JS_DupValue(ctx, c->finish_funcs[s->reject]);
            s->next = S_DONE;
            ts_goto(s, S_SETTLE);
            continue;

        /* ---- the two settles ----------------------------------------------------------------------------- */

        case S_SETTLE: {
            JSValueConst arg = s->w.value;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, JS_UNDEFINED, 1, &arg, cb_result,
                              &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            ts_goto(s, s->next);
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
            ts_goto(s, S_DONE);
            continue;
        }

        default:
            DCHECK(ts_stage(s) == S_DONE,
                   "a §6 machine resumed in a stage §6's operations do not have between them");
            JS_FreeValue(ctx, cb_result);
            if (s->answer == 1) {
                /* the CONSTRUCTOR's answer is the stream itself */
                JS_FreeValue(ctx, s->promise);
                s->promise = JS_DupValue(ctx, s->ts);
            } else if (s->answer == 2) {
                /* the PULL's answer is the change promise the sub-sequence has just made */
                JS_FreeValue(ctx, s->promise);
                s->promise = JS_DupValue(ctx, t->bp_promise);
            } else if (s->rethrow) {
                /* the transform's rejection, reported after the stream has been errored */
                JS_Throw(ctx, JS_DupValue(ctx, s->w.err));
                return JS_STEP_ABRUPT;
            }
            return JS_STEP_DONE;
        }
    }
}

/* The trampoline entry: the five algorithms, §6.3's members and every reaction. */
static int js_ts_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTsState *s = st;
    return ts_run(ctx, s, &s->hdr, s->hdr.arg, cb_result, out_cb, out_argc);
}

/* §6.2's CONSTRUCTOR, declared at the IDL layer so its two QueuingStrategy arguments are converted the way Web
   IDL states — before the body, in argument order. Its answer is the stream. */
static int js_ts_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSTsState *s = st;
    int r;
    (void)argc; (void)argv;
    r = ts_run(ctx, s, hdr, OP_CTOR, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r == JS_STEP_ABRUPT) return -1;
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return 0;
}

/* ONE STATE, ONE OWNERSHIP CONTRACT. The constructor is a different ENTRY, not a different machine — it parks
   the same JSTsState on an IDL declaration instead of a step def — so it names the same visit. A wrapper that
   forwarded to it was a second declaration of the same ownership, which is exactly the shape that lets a field
   get added to one and not the other; the step-visits gate rejects it by struct for that reason. */
static const IdlStepDecl js_ts_ctor_decl = {
    js_ts_ctor_step, sizeof(JSTsState), js_ts_visit, NULL,
    "Streams §6.2 new TransformStream(transformer, writableStrategy, readableStrategy)", TS_STEPS
};

/* ---- the plain accessors --------------------------------------------------------------------------------- */

enum { TS_READABLE = 0, TS_WRITABLE };

static JSValue js_ts_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    TsData *t = ts_of(this_val);
    if (!t) return JS_ThrowTypeError(ctx, "not a TransformStream");
    return JS_DupValue(ctx, magic == TS_READABLE ? t->readable : t->writable);
}

static JSValue js_tc_desired(JSContext *ctx, JSValueConst this_val, int magic)
{
    TsCtrlData *c = tc_of(this_val);
    TsData *t;
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a TransformStreamDefaultController");
    t = ts_of(c->stream);
    DCHECK(t != NULL, "a §6 controller's stream stopped being a TransformStream");
    /* §6.3: the desired size IS the readable half's — this controller is a view onto that one. It performs the
       OPERATION, not the member: reading the accessor from here would run a getter off the tramp chain, and a
       page that replaced §4.5's `desiredSize` must not thereby change what a transform stream answers. */
    return readable_ctrl_desired_size(ctx, readable_stream_controller(t->readable));
}

static JSValue js_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* ---- install --------------------------------------------------------------------------------------------- */

#define TS_DEF(i) { sizeof(JSTsState), js_ts_step, js_ts_fini, (i), .visit = js_ts_visit, \
                    .algorithm = "Streams §6 TransformStream (the shared machine over §6.2-§6.4's operations)", \
                    .steps = TS_STEPS }
/* OP_CTOR has no entry here: the constructor is declared at the IDL layer. Its slot stays so the operation
   numbering is one enum rather than two that must be kept in step. */
static const JSTrampStepDef js_ts_defs[OP_N] = {
    TS_DEF(0),  TS_DEF(1),  TS_DEF(2),  TS_DEF(3),  TS_DEF(4),  TS_DEF(5), TS_DEF(6),
    TS_DEF(7),  TS_DEF(8),  TS_DEF(9),  TS_DEF(10), TS_DEF(11), TS_DEF(12), TS_DEF(13), TS_DEF(14),
    TS_DEF(15),
};
#undef TS_DEF

void transform_stream_init(JSContext *ctx)
{
    JSClassDef sd = { "TransformStream", .finalizer = ts_finalizer, .gc_mark = ts_gc_mark };
    JSClassDef cd = { "TransformStreamDefaultController", .finalizer = tc_finalizer, .gc_mark = tc_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType TS_ARGS[3] = { IDL_ANY, IDL_ANY, IDL_ANY };
    int i;

    DCHECK(g_ts_rt == NULL || g_ts_rt == rt, "TransformStream was installed into a second runtime");
    if (g_ts_rt == rt) return;
    g_ts_rt = rt;
    JS_NewClassID(rt, &g_ts_class);  JS_NewClass(rt, g_ts_class, &sd);
    JS_NewClassID(rt, &g_tc_class);  JS_NewClass(rt, g_tc_class, &cd);

    for (i = 0; i < OP_N; i++) {
        g_op_stepid[i] = JS_RegisterStepDef(rt, &js_ts_defs[i]);
        CHECK(g_op_stepid[i] >= 0, "streams: no step id for a §6 operation");
    }

    /* §6.2's constructor: `(optional object transformer, optional QueuingStrategy writableStrategy = {},
       optional QueuingStrategy readableStrategy = {})`. The two strategies are read by the MACHINE rather than
       declared as IDL dictionaries, because the standard's order runs through them AND the transformer, and
       only one reader can sequence all three. */
    g_ctor_stepid = idl_method_id_step(ctx, TS_ARGS, 3, NULL, 0, &js_ts_ctor_decl, 0);
    idl_optional_from(0);   /* §6.2: all three constructor arguments are optional */

    {
        static const char *const OP_NAME[TS_OP_N] = {
            "§9.3.1 set up", "controller.enqueue", "controller.terminate", "controller.error",
        };
        for (i = 0; i < TS_OP_N; i++)
            g_ts_fn_slot[i] = realm_value_declare(ctx, OP_NAME[i]);
    }
    realm_declare_intrinsic(transform_stream_install_protos);
}

/* §6.2's AND §6.3's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM, and §9.3's four operations read off them. */
void transform_stream_install_protos(JSContext *ctx)
{
    static const char *const CN[3] = { "enqueue", "terminate", "error" };
    JSValue ts_p, tc_p, prev;
    int k;

    DCHECK(g_ts_class != 0, "a realm asked for TransformStream.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_ts_class);
    DCHECK(JS_IsNull(prev), "transform_stream_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    ts_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(ts_p), "TransformStream.prototype could not be allocated");
    idl_interface_tag(ctx, ts_p, "TransformStream");
    idl_install_accessor(ctx, ts_p, "readable", js_ts_get, TS_READABLE, -1);
    idl_install_accessor(ctx, ts_p, "writable", js_ts_get, TS_WRITABLE, -1);
    JS_SetClassProto(ctx, g_ts_class, ts_p);

    tc_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(tc_p), "the §6.3 controller prototype could not be allocated");
    idl_interface_tag(ctx, tc_p, "TransformStreamDefaultController");
    idl_install_accessor(ctx, tc_p, "desiredSize", js_tc_desired, 0, -1);
    idl_install_step_method(ctx, tc_p, "enqueue", 0, g_op_stepid[OP_CTRL_ENQUEUE]);
    idl_install_step_method(ctx, tc_p, "error", 0, g_op_stepid[OP_CTRL_ERROR]);
    idl_install_step_method(ctx, tc_p, "terminate", 0, g_op_stepid[OP_CTRL_TERMINATE]);
    JS_SetClassProto(ctx, g_tc_class, tc_p);

    /* §9.3's four operations as FUNCTION OBJECTS. The three controller ones are read off the prototype rather
       than minted a second time, so a caller reaches the same function the page sees — and if the page
       replaces the property, the reference taken HERE is the one that keeps working, which is the point. */
    {
        JSValue setup = JS_NewCFunction2(ctx, NULL, "", 3, JS_CFUNC_step, g_op_stepid[OP_SETUP]);
        CHECK(!JS_IsException(setup), "streams: §9.3.1's set-up operation could not be made");
        realm_value_set(ctx, g_ts_fn_slot[TS_OP_SETUP], setup);
    }
    for (k = 0; k < 3; k++) {
        JSValue fn = JS_GetPropertyStr(ctx, tc_p, CN[k]);
        CHECK(JS_IsFunction(ctx, fn),
              "streams: a §6.4 controller member this component just installed is not a function");
        realm_value_set(ctx, g_ts_fn_slot[TS_OP_ENQUEUE + k], fn);
    }
}

JSValue transform_stream_op(JSContext *ctx, TransformStreamOp which)
{
    DCHECK(which >= 0 && which < TS_OP_N,
           "a §9.3 transform-stream operation was asked for by a name this component does not map");
    return realm_value_get(ctx, g_ts_fn_slot[which]);   /* OWNED */
}

JSValueConst transform_stream_readable(JSValueConst stream)
{
    TsData *t = ts_of(stream);
    DCHECK(t != NULL, "the readable half of something that is not a TransformStream was asked for");
    return t->readable;
}

JSValueConst transform_stream_writable(JSValueConst stream)
{
    TsData *t = ts_of(stream);
    DCHECK(t != NULL, "the writable half of something that is not a TransformStream was asked for");
    return t->writable;
}

void transform_stream_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ctor_stepid >= 0, "TransformStream was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "TransformStream", 0, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TransformStream interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_ts_class);
        DCHECK(!JS_IsNull(proto), "TransformStream was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "TransformStream", ctor);

    ctor = JS_NewCFunction2(ctx, js_illegal_ctor, "TransformStreamDefaultController", 0,
                            JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the §6.3 controller interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_tc_class);
        DCHECK(!JS_IsNull(proto), "the §6.3 controller was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "TransformStreamDefaultController", ctor);
}

void transform_stream_free(JSContext *ctx)
{
    int i;
    if (!g_ts_rt) return;
    /* the prototypes and the captured operations are the REALMS' — released with their contexts */
    g_ts_rt = NULL;
    g_ctor_stepid = -1;
    for (i = 0; i < OP_N; i++) g_op_stepid[i] = -1;
}
