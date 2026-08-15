/* WEB IDL §3.7.10's DEFAULT ASYNCHRONOUS ITERATOR OBJECT — see core/idl_async_iter.h for what it is and why it
 * is shared. This file is §3.7.10, §3.7.10.1 and §3.7.10.2 and nothing else: the three members an
 * `async_iterable<>` declaration puts on the interface prototype object, the object they hand out, and that
 * object's prototype with its `next` and its `return`. What is ITERATED is entirely the component's — §2.5.10
 * requires the prose accompanying such an interface to define "get the next iteration result", and this file
 * calls it. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_async_iter.h"
#include "solver/cow.h"

/* §3.7.10.1's KIND — "the iteration kind". §3.7.10 mints "key" for `keys`, "value" for `values` and "key+value"
   for `entries` (and for %Symbol.asyncIterator% on a pair declaration, which IS `entries`). */
enum { AIT_KIND_KEY = 0, AIT_KIND_VALUE, AIT_KIND_KEYVALUE };

/* THE SEVEN ENTRIES §3.7.10.2's TWO ALGORITHMS ARE MADE OF. Five of them are built-in function objects the
   standard creates INSIDE those algorithms — `nextSteps`/`returnSteps` reached as an onSettled reaction, and
   the fulfil/reject handlers — so each is reached as a promise reaction, which is what a step closure is for.
   They share one step function because they share one algorithm; WHICH entry this is rides on the definition's
   `arg` beside the interface handle, and there is one definition per (interface, entry) because that is what
   makes step 7's "is object a default asynchronous iterator object FOR INTERFACE" answerable at all:
   `A.prototype.next.call(bAsyncIterator)` must reject even though the receiver is a perfectly good iterator of
   another interface, and a definition shared across interfaces could not tell. */
enum {
    AIT_NEXT = 0,      /* §3.7.10.2's `next` */
    AIT_RETURN,        /* §3.7.10.2's `return` */
    AIT_RUN_NEXT,      /* next step 10.2's onSettled — nextSteps, reached once the ongoing promise settled */
    AIT_RUN_RETURN,    /* return step 10.2's onSettled — returnSteps, likewise */
    AIT_FULFILL,       /* next step 8.6's onFulfilled over step 8.5's fulfillSteps */
    AIT_REJECT,        /* next step 8.8's onRejected over step 8.7's rejectSteps */
    AIT_RET_FULFILL,   /* return step 13's onFulfilled over step 12's fulfillSteps */
    AIT_OP_N
};

/* One declared interface. There are as many as the platform has `async_iterable<>` declarations, which is a
   handful, so the table is fixed and full is a DCHECK rather than a growth path nobody exercises. */
#define IDL_ASYNC_ITER_MAX 8

typedef struct {
    const IdlAsyncIterOps *ops;
    JSClassID class_id;                 /* the iterator object's class, and its per-realm prototype slot */
    int       stepid[AIT_OP_N];
    int       id_keys, id_values, id_entries;
} IdlAsyncIface;

static IdlAsyncIface g_async[IDL_ASYNC_ITER_MAX];
static int  g_async_n;
static bool g_async_sealed;

/* §2.5.10's END OF ITERATION, as a CLASS rather than as a value. A class id is an agent-level integer with no
   lifetime of its own, where a runtime-lifetime JSValue held in a static would be one realm's object answering
   for every document — the defect §3.7's per-realm rule names. Nothing else wears this class, so nothing a page
   can build is ever mistaken for the marker, and `undefined` stays available as a legitimate iterated value. */
static JSClassID g_end_class;

/* THE DEFINITIONS ARE STATIC STORAGE, which is what JS_RegisterStepDef requires ("BORROWED and must outlive the
   runtime"). They are FILLED at declaration rather than written out because the only field that differs among
   them is `arg`. */
static JSTrampStepDef g_defs[IDL_ASYNC_ITER_MAX][AIT_OP_N];

/* AND SO IS THE MEMBER'S — idl_method_id_step BORROWS the IdlStepDecl it is handed. One per interface, because
   the state size it declares is this interface's initialization storage. */
static IdlStepDecl g_make_decl[IDL_ASYNC_ITER_MAX];

#define AIT_HANDLE(arg) ((arg) >> 3)
#define AIT_OP(arg)     ((arg) & 7)

/* §3.7.10.1's DEFAULT ASYNCHRONOUS ITERATOR OBJECT — "an object whose [[Prototype]] internal slot is the
   asynchronous iterator prototype object for the interface", carrying the four internal values that section
   lists. `ongoing` is JS_NULL where the standard says null and never undefined, because steps 10 and 11 branch
   on exactly that. */
typedef struct {
    JSValue target;     /* "its target, which is an object whose values are to be iterated" */
    JSValue ongoing;    /* "its ongoing promise, which is a Promise or null" */
    /* The component's own per-iterator state — §2.5.10: the algorithm "receives ... the async iterator itself
       (which can be useful for storing state)". A JS VALUE, so a flow's writes to it are property writes the
       COW delta already captures and it parks to the cold tier with the flow. */
    JSValue state;
    uint8_t kind;       /* "its kind, which is the iteration kind" */
    uint8_t finished;   /* "its is finished, which is a boolean" */
    uint8_t iface;      /* which declared interface it belongs to */
} IdlAsyncIter;

#define AIT_OFF(f) (uint16_t)offsetof(IdlAsyncIter, f)
static const uint16_t AIT_VALS[] = { AIT_OFF(target), AIT_OFF(ongoing), AIT_OFF(state) };
static const CowRecord AIT_REC = { sizeof(IdlAsyncIter), AIT_VALS, (int)(sizeof AIT_VALS / sizeof *AIT_VALS) };

static void idl_async_iter_finalizer(JSRuntime *rt, JSValue val)
{
    int i;

    for (i = 0; i < g_async_n; i++) {
        IdlAsyncIter *it = JS_GetOpaque(val, g_async[i].class_id);
        if (it) {
            JS_FreeValueRT(rt, it->target);
            JS_FreeValueRT(rt, it->ongoing);
            JS_FreeValueRT(rt, it->state);
            js_free_rt(rt, it);
            return;
        }
    }
}

/* THE RECORD IS A CYCLE WAITING TO HAPPEN — `for await (const [name, h] of dir)` puts the iterator inside a
   closure the target can reach — so it is marked like every other component record that holds values. It goes
   through JS_GetOpaque rather than through the accessor below on purpose: a COW capture during collection would
   dup values on an object being torn down. */
static void idl_async_iter_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    int i;

    for (i = 0; i < g_async_n; i++) {
        IdlAsyncIter *it = JS_GetOpaque(val, g_async[i].class_id);
        if (it) {
            JS_MarkValue(rt, it->target, mark_func);
            JS_MarkValue(rt, it->ongoing, mark_func);
            JS_MarkValue(rt, it->state, mark_func);
            return;
        }
    }
}

/* THE RECORD'S ACCESSOR, AND THEREFORE ITS COW CAPTURE POINT. `ongoing promise`, `is finished` and the
   component's state are written by §3.7.10.2 and by the component's algorithms, and a flow that has REACHED the
   record is one that may write it — so the capture belongs here rather than at each of the assignments, where
   the last one added would be the one forgotten. */
static IdlAsyncIter *ait_of(JSValueConst v)
{
    int i;

    for (i = 0; i < g_async_n; i++) {
        IdlAsyncIter *it = JS_GetOpaque(v, g_async[i].class_id);
        if (it) { cow_capture_host_record(v, it, &AIT_REC); return it; }
    }
    return NULL;
}

JSValue idl_async_iter_end(JSContext *ctx)
{
    DCHECK(g_end_class != 0, "§2.5.10's end of iteration was asked for before any async_iterable<> declared "
                             "itself — the class is minted by the first declaration");
    return JS_NewObjectClass(ctx, g_end_class);
}

/* 7.4.14 CreateIteratorResultObject(value, done) — OrdinaryObjectCreate(%Object.prototype%) and two
   CreateDataPropertyOrThrow. DEFINE and not Set: a page that installs a setter for "value" or "done" on
   Object.prototype must not see it run and must not be able to swallow the write. CONSUMES `value`. */
static JSValue ait_iter_result(JSContext *ctx, JSValue value, bool done)
{
    JSValue res = JS_NewObject(ctx);

    if (JS_IsException(res)) { JS_FreeValue(ctx, value); return res; }
    if (JS_DefinePropertyValueStr(ctx, res, "value", value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, res, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);       /* both defines CONSUME their value, so nothing is left behind here */
        return JS_EXCEPTION;
    }
    return res;
}

/* ---- the machine ----------------------------------------------------------------------------------------- */

/* WHERE THIS MACHINE RESTS, AS §3.7.10.2 NUMBERS ITS TWO ALGORITHMS. `next` and `return` share the list because
   they share the shape: check the receiver, chain onto the ongoing promise or run the steps here, short-circuit
   on `is finished`, ask the component, settle. */
#define AIT_STAGES(X) \
    X(AIT_START, \
      "Web IDL §3.7.10.2 next steps 1-11 / return steps 1-11 (the receiver check, and then either chaining " \
      "this call onto an outstanding ongoing promise or running nextSteps/returnSteps here)") \
    X(AIT_REJECT_THIS, \
      "Web IDL §3.7.10.2 next step 7.2 / return step 7.2 (Call(thisValidationPromiseCapability.[[Reject]], " \
      "undefined, « a new TypeError ») — a resolving function reaches 27.2.1.3.2 step 8's `then` read)") \
    X(AIT_STEPS, \
      "Web IDL §3.7.10.2 next step 8.2 / return steps 8.2-8.3 (whether `is finished` short-circuits this " \
      "iteration step, and the return algorithm's setting of it)") \
    X(AIT_FINISHED, \
      "Web IDL §3.7.10.2 next step 8.2.2 / return step 8.2.2 (Call(nextPromiseCapability.[[Resolve]], " \
      "undefined, « CreateIteratorResultObject(...) »))") \
    X(AIT_SOURCE, \
      "Web IDL §3.7.10.2 next step 8.4 (getting the next iteration result with the target and this iterator) " \
      "/ return step 8.4 (running the asynchronous iterator return algorithm for the interface)")
enum { AIT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const AIT_STEP_LABELS[] = { AIT_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The component's own step storage sits in the TAIL of this state, so one allocation carries both and the
   component never owns a second lifetime. The union is what makes that tail's alignment the widest of anything
   a machine may put there rather than this struct's last member's. */
typedef union { JSValue v; double d; void *p; int64_t i; } AitAlign;

typedef struct {
    JSStepHdr hdr;
    uint8_t   phase;     /* step_call_run's cursor, for whichever settle is in flight */
    JSValue   iter;      /* the default asynchronous iterator object this entry is about (owned) */
    JSValue   result;    /* what this entry answers with — fini hands it back (owned) */
    JSValue   func;      /* a capability's [[Resolve]] or [[Reject]], while a settle is in flight (owned) */
    JSValue   value;     /* what that settle passes; for `return`, the argument it was given (owned) */
    JSValue   source;    /* the promise the component's algorithm returned (owned) */
    JSValue   cb[3];     /* step_call_run's buffer: 2 + the one argument a settle takes */
    AitAlign  work[1];   /* the component's own storage begins here */
} AitState;

#define AIT_WORK(s) ((void *)(s)->work)

static size_t ait_state_size(size_t work_size)
{
    return sizeof(AitState) + (work_size > sizeof(AitAlign) ? work_size - sizeof(AitAlign) : 0);
}

/* THE ONE DECLARATION OF WHAT THIS STATE OWNS — read by the deep-fork clone and by the teardown alike, so the
   two cannot disagree about which fields those are. The component's tail is part of it, declared by the
   component for the same reason. */
static void ait_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    AitState *s = st;
    const IdlAsyncIterOps *ops = g_async[AIT_HANDLE(s->hdr.arg)].ops;
    int k;

    v->val(ctx, &s->iter);
    v->val(ctx, &s->result);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    v->val(ctx, &s->source);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
    if (ops->work_visit) ops->work_visit(ctx, AIT_WORK(s), v);
}

static JSValue ait_fini(JSContext *ctx, void *st, bool take_result)
{
    AitState *s = st;
    JSValue r = JS_UNDEFINED;

    if (take_result) { r = s->result; s->result = JS_UNDEFINED; }
    JS_StepVisitFree(ctx, ait_visit, st);
    return r;
}

/* §3.7.10.2 return steps 12-14: fulfillSteps answers CreateIteratorResultObject(value, true), attached to the
   ongoing promise with NO reject handler — so a rejection out of the asynchronous iterator return algorithm
   reaches the caller unchanged, which is §2.5.10's "if it rejects, that failure will be passed on to users of
   the async iterator API". */
static int ait_return_tail(JSContext *ctx, AitState *s, IdlAsyncIter *rec)
{
    const IdlAsyncIface *f = &g_async[AIT_HANDLE(s->hdr.arg)];
    JSValueConst data[1];
    JSValue onf, cap;

    DCHECK(!JS_IsNull(rec->ongoing),
           "§3.7.10.2 return step 14 attaches to `object's ongoing promise`, and steps 10-11 have just set it "
           "— a null here means neither of them ran");
    data[0] = s->value;
    onf = JS_NewStepClosure(ctx, f->stepid[AIT_RET_FULFILL], 1, 1, data);
    if (JS_IsException(onf)) return JS_STEP_ABRUPT;
    cap = JS_PerformPromiseThen(ctx, rec->ongoing, onf, JS_UNDEFINED);
    JS_FreeValue(ctx, onf);
    if (JS_IsException(cap)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, s->result);
    s->result = cap;                     /* step 15: return returnPromiseCapability.[[Promise]] */
    return JS_STEP_DONE;
}

/* §3.7.10.2's fulfillSteps for `next` (step 8.5), minus its first step: what the component's promise resolved
   with, read as either end-of-iteration or a value of the declaration's shape. Returns the iteration result. */
static JSValue ait_fulfil_result(JSContext *ctx, const IdlAsyncIface *f, IdlAsyncIter *rec, JSValueConst next)
{
    JSValue key, value, arr;

    if (JS_GetClassID(next) == g_end_class) {         /* step 8.5.2 */
        rec->finished = 1;
        return ait_iter_result(ctx, JS_UNDEFINED, true);
    }
    if (!f->ops->pair)                                /* step 8.5.4 */
        return ait_iter_result(ctx, JS_DupValue(ctx, next), false);

    /* step 8.5.3: "Assert: next is a value pair" — which at this layer is the two-element Array the component's
       algorithm resolved with. It is the component's own object, so reading it runs none of the page's code. */
    DCHECK(JS_IsArray(next), "a pair async_iterable<>'s get the next iteration result resolved with something "
                             "that is not a value pair — §2.5.10 requires a TUPLE of the declaration's two "
                             "types, or the end of iteration value");
    key   = JS_GetPropertyUint32(ctx, next, 0);
    value = JS_GetPropertyUint32(ctx, next, 1);
    /* §3.7.9.2's "the iterator result for a value pair pair and a kind kind", which step 8.5.3.2 reaches: the
       key, the value, or a NEW two-element Array of both. NEW, because the component's tuple is the component's
       own object and handing it out would make two iteration results alias one array. */
    if (rec->kind == AIT_KIND_KEY)   { JS_FreeValue(ctx, value); return ait_iter_result(ctx, key, false); }
    if (rec->kind == AIT_KIND_VALUE) { JS_FreeValue(ctx, key);   return ait_iter_result(ctx, value, false); }
    DCHECK(rec->kind == AIT_KIND_KEYVALUE,
           "a default asynchronous iterator object carries a kind §3.7.10 does not mint");
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { JS_FreeValue(ctx, key); JS_FreeValue(ctx, value); return arr; }
    /* Two defines and two checks, because each CONSUMES its value: folded into one `||` the second value is
       never handed over when the first fails, and leaks. */
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, key, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, arr, 1, value, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return ait_iter_result(ctx, arr, false);
}

static int ait_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    AitState *s = st;
    int handle = AIT_HANDLE(s->hdr.arg), op = AIT_OP(s->hdr.arg);
    const IdlAsyncIface *f = &g_async[handle];
    bool returning = op == AIT_RETURN || op == AIT_RUN_RETURN;
    IdlAsyncIter *rec;
    JSValue funcs[2];
    int r;

    if (s->hdr.stage == AIT_START) {
        int k;

        /* A ZEROED SLOT READS AS THE INTEGER 0, not as undefined — so every owned slot is made undefined before
           the first thing that can fail, because the failure path tears this state down through the declaration
           above and frees exactly what that names. */
        s->iter = s->result = s->func = s->value = s->source = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        if (op == AIT_RET_FULFILL) {
            /* return step 12.1: "Return CreateIteratorResultObject(value, true)" — `value` is the argument the
               member was given, captured here because the member returned long before this runs. */
            s->result = ait_iter_result(ctx, JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0)), true);
            return JS_IsException(s->result) ? JS_STEP_ABRUPT : JS_STEP_DONE;
        }

        if (op == AIT_FULFILL || op == AIT_REJECT) {
            s->iter = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            rec = ait_of(s->iter);
            DCHECK(rec != NULL, "a §3.7.10.2 reaction captured something that is not a default asynchronous "
                                "iterator object — it was captured at the ask, so it cannot have become one");
            JS_FreeValue(ctx, rec->ongoing);
            rec->ongoing = JS_NULL;                        /* steps 8.5.1 and 8.7.1 */
            if (op == AIT_REJECT) {
                rec->finished = 1;                         /* step 8.7.2 */
                /* step 8.7.3: "Throw reason" — which rejects the capability step 8.9 attached this to, so the
                   reason reaches the page's `catch` unchanged rather than re-wrapped. */
                JS_Throw(ctx, JS_DupValue(ctx, step_arg(&s->hdr, 0)));
                return JS_STEP_ABRUPT;
            }
            s->result = ait_fulfil_result(ctx, f, rec, step_arg(&s->hdr, 0));
            if (JS_IsException(s->result)) return JS_STEP_ABRUPT;
            DCHECK(JS_GetClassID(s->result) != g_end_class,
                   "§2.5.10's end of iteration value escaped into an iteration result — it is a marker between "
                   "a component's algorithm and this file, and a page must never be handed one");
            return JS_STEP_DONE;
        }

        if (op == AIT_NEXT || op == AIT_RETURN) {
            /* steps 3-4: the this value. There is no separate ToObject to perform — a primitive receiver
               carries no opaque, so step 7's check is what rejects it, with the same TypeError. */
            s->iter = JS_DupValue(ctx, s->hdr.this_val);
            rec = ait_of(s->iter);
            if (!rec || rec->iface != handle) {
                /* step 7: "If object is not a default asynchronous iterator object for interface" — REJECT the
                   validation capability rather than throw, because a page distinguishes the two and every
                   caller of `next` is written for a promise. */
                s->result = JS_NewPromiseCapability(ctx, funcs);           /* step 2 */
                if (JS_IsException(s->result)) return JS_STEP_ABRUPT;
                s->func = funcs[1];
                JS_FreeValue(ctx, funcs[0]);
                JS_ThrowTypeError(ctx, "%s AsyncIterator.prototype.%s was called on something that is not one",
                                  f->ops->iface, op == AIT_NEXT ? "next" : "return");
                s->value = JS_GetException(ctx);
                CHECK(!JS_IsUndefined(s->value),
                      "an asynchronous iterator's receiver check built a TypeError and none was live");
                STEP_GOTO(s->hdr.stage, AIT_REJECT_THIS, &s->phase, NULL);
            } else {
                if (op == AIT_RETURN)
                    s->value = JS_DupValue(ctx, step_arg(&s->hdr, 0));
                if (!JS_IsNull(rec->ongoing)) {
                    /* STEPS 9-11, THE ORDERING RULE. A call made while a previous one is still in flight does
                       not start a second iteration step: nextSteps/returnSteps is attached to the outstanding
                       ongoing promise as BOTH handlers — so it runs whichever way that one settles — and the
                       ongoing promise becomes the promise of THAT reaction, so the calls form a chain. */
                    JSValueConst data[2];
                    JSValue onsettled, cap;
                    int nd = 0;

                    data[nd++] = s->iter;
                    if (op == AIT_RETURN) data[nd++] = s->value;
                    /* step 10.2: CreateBuiltinFunction(nextSteps, 0, "", « ») — a LENGTH of 0, because the
                       settled value of the ongoing promise is not one of nextSteps' inputs. */
                    onsettled = JS_NewStepClosure(ctx,
                                                  f->stepid[op == AIT_NEXT ? AIT_RUN_NEXT : AIT_RUN_RETURN],
                                                  0, nd, data);
                    if (JS_IsException(onsettled)) return JS_STEP_ABRUPT;
                    cap = JS_PerformPromiseThen(ctx, rec->ongoing, onsettled, onsettled);   /* step 10.3 */
                    JS_FreeValue(ctx, onsettled);
                    if (JS_IsException(cap)) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, rec->ongoing);
                    rec->ongoing = cap;                                                     /* step 10.4 */
                    if (op == AIT_NEXT) {
                        s->result = JS_DupValue(ctx, rec->ongoing);                         /* step 12 */
                        return JS_STEP_DONE;
                    }
                    return ait_return_tail(ctx, s, rec);                                    /* steps 12-15 */
                }
                STEP_GOTO(s->hdr.stage, AIT_STEPS, &s->phase, NULL);   /* step 11: run the steps here */
            }
        } else {
            DCHECK(op == AIT_RUN_NEXT || op == AIT_RUN_RETURN,
                   "a §3.7.10.2 machine ran with an entry this file does not have");
            /* step 10.2's onSettled: nextSteps/returnSteps, whose `object` and (for return) `value` are the
               ones the member closed over. */
            s->iter = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            if (op == AIT_RUN_RETURN) s->value = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            STEP_GOTO(s->hdr.stage, AIT_STEPS, &s->phase, NULL);
        }
    }

    if (s->hdr.stage == AIT_REJECT_THIS) {
        JSValue settled = JS_UNDEFINED;

        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1,
                          (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (r > 0) return r;
        DCHECK(!JS_IsException(settled),
               "a promise capability's [[Reject]] threw — 27.2.1.3.1 answers undefined for every input, which "
               "is why §3.7.10.2 marks this Call `!`");
        JS_FreeValue(ctx, settled);
        return JS_STEP_DONE;               /* step 7.3 */
    }

    rec = ait_of(s->iter);
    DCHECK(rec != NULL, "a §3.7.10.2 machine reached its steps holding something that is not a default "
                        "asynchronous iterator object");

    if (s->hdr.stage == AIT_STEPS) {
        if (rec->finished) {
            /* next step 8.2 / return step 8.2 — the only thing an exhausted iterator does. `return` answers
               with the ARGUMENT it was given rather than with undefined; `next` with undefined. */
            JSValue res;

            s->result = JS_NewPromiseCapability(ctx, funcs);      /* step 8.1 */
            if (JS_IsException(s->result)) return JS_STEP_ABRUPT;
            s->func = funcs[0];
            JS_FreeValue(ctx, funcs[1]);
            res = ait_iter_result(ctx, returning ? JS_DupValue(ctx, s->value) : JS_UNDEFINED, true);
            if (JS_IsException(res)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->value);
            s->value = res;
            STEP_GOTO(s->hdr.stage, AIT_FINISHED, &s->phase, NULL);
        } else {
            if (returning) rec->finished = 1;                     /* return step 8.3 */
            STEP_GOTO(s->hdr.stage, AIT_SOURCE, &s->phase, NULL);
        }
    }

    if (s->hdr.stage == AIT_FINISHED) {
        JSValue settled = JS_UNDEFINED;

        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1,
                          (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (r > 0) return r;
        DCHECK(!JS_IsException(settled),
               "a promise capability's [[Resolve]] threw — 27.2.1.3.2 rejects rather than throwing for every "
               "input, including a thenable whose `then` getter throws, which is why this Call is `!`");
        JS_FreeValue(ctx, settled);
        cb_result = JS_UNDEFINED;
        /* step 8.2.3: return nextPromiseCapability.[[Promise]], which s->result already holds */
    } else {
        DCHECK(s->hdr.stage == AIT_SOURCE,
               "a §3.7.10.2 machine resumed into a stage neither of its algorithms has");
        if (returning) {
            DCHECK(f->ops->ret != NULL,
                   "§3.7.10.2's `return` ran for an interface whose prose defines no asynchronous iterator "
                   "return algorithm — the member is only installed when there is one");
            r = f->ops->ret(ctx, &s->hdr, AIT_WORK(s), rec->target, s->iter, &rec->state, s->value,
                            cb_result, &s->source, out_cb, out_argc);
        } else {
            r = f->ops->next(ctx, &s->hdr, AIT_WORK(s), rec->target, s->iter, &rec->state,
                             cb_result, &s->source, out_cb, out_argc);
        }
        if (r > 0) return r;                 /* parked INSIDE the component's algorithm; the resume returns here */
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        /* THE RECORD MAY HAVE MOVED UNDER US: the component's algorithm can suspend, and a resume re-derives
           every pointer. Re-fetch rather than trusting the one taken before the call. */
        rec = ait_of(s->iter);
        DCHECK(rec != NULL, "a §3.7.10.2 machine's iterator stopped being one across its component's algorithm");
        if (returning) {
            s->result = s->source;           /* return step 8.4: the algorithm's promise IS returnSteps' result */
            s->source = JS_UNDEFINED;
        } else {
            /* steps 8.5-8.10. PerformPromiseThen's own capability IS nextPromiseCapability: the standard
               creates one in step 8.1 and hands it to step 8.9, and it is unobservable until step 8.10 returns
               it, so creating it where it is used is the same promise by a shorter route. */
            JSValueConst data[1];
            JSValue onf, onr, cap;

            data[0] = s->iter;
            onf = JS_NewStepClosure(ctx, f->stepid[AIT_FULFILL], 1, 1, data);
            if (JS_IsException(onf)) return JS_STEP_ABRUPT;
            onr = JS_NewStepClosure(ctx, f->stepid[AIT_REJECT], 1, 1, data);
            if (JS_IsException(onr)) { JS_FreeValue(ctx, onf); return JS_STEP_ABRUPT; }
            cap = JS_PerformPromiseThen(ctx, s->source, onf, onr);
            JS_FreeValue(ctx, onf);
            JS_FreeValue(ctx, onr);
            if (JS_IsException(cap)) return JS_STEP_ABRUPT;
            s->result = cap;                 /* step 8.10 */
        }
    }

    /* step 11.1: "Set object's ongoing promise to the result of running nextSteps/returnSteps" — for the
       MEMBER only. An entry reached as step 10.2's onSettled had the ongoing promise set by step 10.4 before it
       was ever queued, and overwriting it here would drop the chain a third call is already waiting on. */
    if (op == AIT_NEXT || op == AIT_RETURN) {
        JS_FreeValue(ctx, rec->ongoing);
        rec->ongoing = JS_DupValue(ctx, s->result);
    }
    if (op == AIT_RETURN) return ait_return_tail(ctx, s, rec);   /* steps 12-15 */
    return JS_STEP_DONE;                                         /* next step 12 / a reaction's own answer */
}

/* ---- §3.7.10's three members ------------------------------------------------------------------------------ */

/* WHERE THE MEMBER RESTS, AS §3.7.10 NUMBERS ITS THREE IDENTICAL STEP LISTS. It is a MACHINE and not a plain
   body because of the second stage: §2.5.10's initialization steps are the COMPONENT'S, and what a standard
   writes into them is not this file's to bound — Streams §4.2.5's first step acquires a reader, whose §4.3
   acquisition settles a promise. */
#define AIM_STAGES(X) \
    X(AIM_MINT, \
      "Web IDL §3.7.10 steps 3.1.1-3.1.5 / 4.1.1-4.1.5 / 5.1.1-5.1.5 (the receiver, its \"does not implement " \
      "definition\" TypeError, and the newly created default asynchronous iterator object for the interface)") \
    X(AIM_INIT, \
      "Web IDL §3.7.10 step 3.1.6 / 4.1.6 / 5.1.6 (running the asynchronous iterator initialization steps for " \
      "the interface with idlObject, iterator and idlArgs)")
enum { IDL_STEP_STAGE_BASE(AIM_STAGES) AIM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const AIM_STEP_LABELS[] = { AIM_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The member's state, with the initialization steps' own storage in its TAIL — one allocation carries both, for
   the reason AitState carries the other two algorithms' the same way. */
typedef struct {
    JSValue  iter;      /* §3.7.10.1's object, owned until step 3.1.7 hands it back */
    uint8_t  handle;    /* which declared interface, so the ownership declaration can reach its work_visit */
    uint8_t  minted;    /* whether the tail below is the component's storage yet, or still zeroed bytes */
    AitAlign work[1];   /* the initialization steps' own storage begins here */
} AitMakeState;

#define AIM_WORK(s) ((void *)(s)->work)

static size_t ait_make_state_size(size_t work_size)
{
    return sizeof(AitMakeState) + (work_size > sizeof(AitAlign) ? work_size - sizeof(AitAlign) : 0);
}

/* THE ONE DECLARATION OF WHAT THIS STATE OWNS. The tail is visited only once the mint has run: a member torn
   down by a THROWING ARGUMENT CONVERSION never reached this body at all, and its tail is then zeroed bytes
   rather than the component's record — which is ownership that is conditional, and therefore an `if` around the
   visit rather than a second list. */
static void ait_make_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    AitMakeState *s = st;

    v->val(ctx, &s->iter);
    if (s->minted) {
        const IdlAsyncIterOps *ops = g_async[s->handle].ops;
        if (ops->work_visit) ops->work_visit(ctx, AIM_WORK(s), v);
    }
}

/* §3.7.10 steps 3.1, 4.1 and 5.1 — one body, because the three differ only in the KIND they mint. `magic` packs
   the interface handle and that kind, since one C function serves every declared interface. */
static int js_idl_async_make(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    AitMakeState *s = st;
    int magic = idl_step_magic(hdr), handle = magic >> 2, kind = magic & 3;
    const IdlAsyncIface *f = &g_async[handle];
    IdlAsyncIter *it;
    int r;

    if (hdr->stage == AIM_MINT) {
        JSValue obj, proto;

        /* A ZEROED SLOT READS AS THE INTEGER 0, not as undefined — so the owned slot is made undefined before
           the first thing that can fail, because the failure path tears this state down through the
           declaration above and frees exactly what that names. */
        s->iter = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        DCHECK(handle >= 0 && handle < g_async_n,
               "an async_iterable<> member ran with a handle nothing declared");
        /* step 3.1.3: "If jsValue does not implement definition, then throw a TypeError." */
        if (!f->ops->implements(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "not a %s", f->ops->iface);
            return -1;
        }
        /* §3.7.10.1's [[Prototype]] is THIS REALM'S asynchronous iterator prototype object — it inherits
           %AsyncIteratorPrototype%, which is a per-realm intrinsic, so one shared object would give a child
           document's `dir.values()` the parent realm's async-iterator helpers. */
        proto = JS_GetClassProto(ctx, f->class_id);
        DCHECK(!JS_IsNull(proto),
               "an async_iterable<>'s iterator was minted in a realm that never ran its install — the member "
               "and the prototype are installed together, so a mint here means one arrived without the other");
        obj = JS_NewObjectProtoClass(ctx, proto, f->class_id);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(obj)) return -1;
        it = js_mallocz(ctx, sizeof *it);
        if (!it) { JS_FreeValue(ctx, obj); return -1; }
        /* step 3.1.5: "a newly created default asynchronous iterator object ... with idlObject as its target,
           ... as its kind, and is finished set to false". The record is COMPLETE before it is attached. */
        it->target = JS_DupValue(ctx, hdr->this_val);
        it->ongoing = JS_NULL;
        it->state = JS_UNDEFINED;
        it->kind = (uint8_t)kind;
        it->finished = 0;
        it->iface = (uint8_t)handle;
        JS_SetOpaque(obj, it);
        s->iter = obj;
        s->handle = (uint8_t)handle;
        s->minted = 1;
        if (!f->ops->init) {
            *presult = s->iter;               /* step 3.1.7: "Return iterator." */
            s->iter = JS_UNDEFINED;
            return 0;
        }
        STEP_GOTO(hdr->stage, AIM_INIT, NULL);
    }

    DCHECK(hdr->stage == AIM_INIT,
           "an async_iterable<> member resumed into a stage §3.7.10's three step lists do not have");
    it = JS_GetOpaque(s->iter, f->class_id);
    DCHECK(it != NULL,
           "an async_iterable<> member's initialization steps ran on something that is not the default "
           "asynchronous iterator object the stage before it minted");
    /* step 3.1.6: "Run the asynchronous iterator initialization steps for definition ... if any such steps
       exist." The state slot is written directly rather than through the capture accessor because this object
       was created by the running flow: it is flow-private, and the delta captures only shared baseline state. */
    r = f->ops->init(ctx, hdr, AIM_WORK(s), hdr->this_val, s->iter, argc, argv, &it->state,
                     cb_result, out_cb, out_argc);
    if (r != 0) return r;                     /* >0 parked inside the component's steps; <0 threw */
    *presult = s->iter;                       /* step 3.1.7: "Return iterator." */
    s->iter = JS_UNDEFINED;
    return 0;
}

/* ---- declaration and per-realm install --------------------------------------------------------------------- */

int idl_async_iter_declare(JSContext *ctx, const IdlAsyncIterOps *ops)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int handle = g_async_n, i;
    IdlAsyncIface *f;
    JSClassDef def;
    char name[96];

    DCHECK(!g_async_sealed,
           "an async_iterable<> was declared after a realm had already been built — every realm's install runs "
           "the declarations that existed when it ran, so a late one is a class no live realm has a prototype "
           "for");
    DCHECK(g_async_n < IDL_ASYNC_ITER_MAX,
           "more async_iterable<> interfaces were declared than this table holds — grow it, the count is fixed "
           "because the platform's is");
    DCHECK(ops != NULL && ops->iface != NULL && ops->next != NULL && ops->implements != NULL,
           "an async_iterable<> declaration is missing the identifier, the receiver check, or §2.5.10's get "
           "the next iteration result — the standard requires the prose to define that last one");
    DCHECK(ops->work_size == 0 || ops->work_visit != NULL,
           "an async_iterable<>'s algorithms were given step storage with no declaration of what it owns — "
           "without one the fork hands two flows one state and the teardown frees none of it");
    DCHECK(ops->nmembers == 0 || ops->members != NULL,
           "an async_iterable<>'s argument list declares a dictionary with no members");

    f = &g_async[handle];
    f->ops = ops;

    /* §2.5.10's END OF ITERATION, minted with the first declaration: agent-level, so every interface's
       algorithms speak the same marker and it belongs to no realm. */
    if (g_end_class == 0) {
        JSClassDef ed = { "end of iteration" };
        JS_NewClassID(rt, &g_end_class);
        CHECK(JS_NewClass(rt, g_end_class, &ed) == 0,
              "§2.5.10's end of iteration marker class could not be declared");
    }

    snprintf(name, sizeof name, "%s AsyncIterator", ops->iface);
    memset(&def, 0, sizeof def);
    def.class_name = name;      /* JS_NewClass copies it */
    def.finalizer = idl_async_iter_finalizer;
    def.gc_mark = idl_async_iter_gc_mark;
    JS_NewClassID(rt, &f->class_id);
    CHECK(JS_NewClass(rt, f->class_id, &def) == 0,
          "an async_iterable<>'s iterator class could not be declared");

    for (i = 0; i < AIT_OP_N; i++) {
        JSTrampStepDef *d = &g_defs[handle][i];

        d->size = ait_state_size(ops->work_size);
        d->step = ait_step;
        d->fini = ait_fini;
        d->arg = (handle << 3) | i;
        d->visit = ait_visit;
        d->algorithm = "Web IDL §3.7.10.2 an asynchronous iterator prototype object's next and return";
        d->steps = AIT_STEP_LABELS;
        f->stepid[i] = JS_RegisterStepDef(rt, d);
        CHECK(f->stepid[i] >= 0, "no step id for an async_iterable<>'s iteration steps");
    }

    {
        /* A member with no declared arguments still needs a type array to hand over; §3.7.10's "converting
           arguments for an asynchronous iterator method" then converts nothing, which is what an empty
           argument list means. */
        static const IdlArgType NONE[1] = { IDL_ANY };
        const IdlArgType *types = ops->nargs > 0 ? ops->arg_types : NONE;
        /* §3.7.10 defines `entries` and `keys` ONLY for a pair declaration ("If definition has a pair
           asynchronously iterable declaration, then define ..."), so a VALUE declaration declares neither —
           a member registered here and installed nowhere would be a pool entry no prototype can reach. */
        int nk = ops->pair ? 3 : 1, k;
        /* THE DECLARATION IS BORROWED by the pool, so it is static storage per interface — the same reason
           g_defs above is, and per interface because the initialization steps' storage is sized from THIS
           declaration's `work_size`. */
        IdlStepDecl *decl = &g_make_decl[handle];
        struct { int kind; int *pid; } m[3] = {
            { AIT_KIND_VALUE,    &f->id_values  },
            { AIT_KIND_KEYVALUE, &f->id_entries },
            { AIT_KIND_KEY,      &f->id_keys    },
        };

        DCHECK(ops->nargs == 0 || ops->arg_types != NULL,
               "an async_iterable<> declared an argument list with no types");
        decl->body       = js_idl_async_make;
        decl->state_size = ait_make_state_size(ops->work_size);
        decl->visit      = ait_make_visit;
        decl->release    = NULL;
        decl->algorithm  = "Web IDL §3.7.10 an asynchronously iterable declaration's values, entries and keys";
        decl->steps      = AIM_STEP_LABELS;
        f->id_values = f->id_entries = f->id_keys = -1;
        for (k = 0; k < nk; k++) {
            int magic = (handle << 2) | m[k].kind;

            *m[k].pid = idl_method_id_step(ctx, types, ops->nargs, ops->members, ops->nmembers, decl, magic);
            /* §2.5.10: "If given, an asynchronously iterable declaration's arguments must all be optional
               arguments." Stated HERE and not by the component, because it is the standard's requirement
               rather than any one interface's — and it is load-bearing: `for await (const x of obj)` calls
               %Symbol.asyncIterator% with none, so a required argument would break the syntax the declaration
               exists to support. */
            if (ops->nargs > 0) idl_optional_from(0);
        }
    }

    g_async_n++;
    return handle;
}

void idl_async_iter_install(JSContext *ctx, JSValueConst proto, int handle)
{
    IdlAsyncIface *f;
    JSValue intrinsic, aproto, prev, entries;
    char name[96];

    DCHECK(handle >= 0 && handle < g_async_n,
           "an async_iterable<> was installed with a handle nothing declared");
    f = &g_async[handle];
    g_async_sealed = true;

    prev = JS_GetClassProto(ctx, f->class_id);
    DCHECK(JS_IsNull(prev),
           "an async_iterable<>'s asynchronous iterator prototype object was built twice in one realm — every "
           "iterator already handed out would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* §3.7.10.2: "The [[Prototype]] internal slot of an asynchronous iterator prototype object must be
       %AsyncIteratorPrototype%." That inheritance is where `@@asyncIterator` returning `this` comes from — so
       `for await (const e of dir.entries())` works — and where the async-iterator helpers come from, so none
       of it is re-declared here. */
    intrinsic = JS_GetAsyncIteratorPrototype(ctx);
    aproto = JS_NewObjectProto(ctx, intrinsic);
    JS_FreeValue(ctx, intrinsic);
    CHECK(!JS_IsException(aproto), "an asynchronous iterator prototype object could not be allocated");

    /* §3.7.10.2 gives `next` all three attributes — ENUMERABLE included, unlike an interface prototype's
       members — which is the descriptor JS_SetPropertyStr defines. */
    idl_install_step_method(ctx, aproto, "next", 0, f->stepid[AIT_NEXT]);
    /* "If an asynchronous iterator return algorithm is defined for the interface, then the asynchronous
       iterator prototype object must have a return data property" — and if not, it must not. A `break` out of
       a `for await` loop then runs AsyncIteratorClose, whose GetMethod finds undefined and calls nothing,
       which is the observable difference. */
    if (f->ops->ret)
        idl_install_step_method(ctx, aproto, "return", 1, f->stepid[AIT_RETURN]);
    /* §3.7.10.2: "The class string of an asynchronous iterator prototype object for a given interface is the
       result of concatenating the identifier of the interface and the string ' AsyncIterator'." §3.7.10.1's
       note makes it the ITERATOR's class string too, which is why the iterator carries none of its own. */
    snprintf(name, sizeof name, "%s AsyncIterator", f->ops->iface);
    JS_DefinePropertyValue(ctx, aproto, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG),
                           JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_SetClassProto(ctx, f->class_id, aproto);

    /* §3.7.10's members on the interface prototype object. A VALUE declaration gets `values` and
       %Symbol.asyncIterator% only; a PAIR declaration gets `entries` and `keys` as well. */
    idl_install_method(ctx, proto, "values", 0, f->id_values);
    if (f->ops->pair) {
        idl_install_method(ctx, proto, "entries", 0, f->id_entries);
        idl_install_method(ctx, proto, "keys", 0, f->id_keys);
    }
    /* §3.7.10 step 3.11 / step 5.7: DefineMethodProperty(target, %Symbol.asyncIterator%, F, false) — the SAME
       function object as `entries` for a pair declaration and as `values` for a value one, so
       `dir[Symbol.asyncIterator] === dir.entries`, and `false` is the [[Enumerable]] it is defined with. Which
       one it is comes off the DECLARATION, never off the caller. */
    entries = JS_GetPropertyStr(ctx, proto, f->ops->pair ? "entries" : "values");
    CHECK(JS_IsFunction(ctx, entries),
          "an async_iterable<>'s %Symbol.asyncIterator% was taken before the member it is the same object as");
    JS_DefinePropertyValue(ctx, proto, JS_WellKnownSymbolAtom(JS_WKS_ASYNC_ITERATOR),
                           entries, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
