/* return JS_ATOM_NULL if the name cannot be found. Only works with
   not striped bytecode functions. */
JSAtom JS_GetScriptOrModuleName(JSContext *ctx, int n_stack_levels)
{
    JSStackFrame *sf;
    JSFunctionBytecode *b;
    JSObject *p;
    /* XXX: currently we just use the filename of the englobing
       function. It does not work for eval(). Need to add a
       ScriptOrModule info in JSFunctionBytecode */
    sf = ctx->rt->current_stack_frame;
    if (!sf)
        return JS_ATOM_NULL;
    while (n_stack_levels-- > 0) {
        sf = sf->prev_frame;
        if (!sf)
            return JS_ATOM_NULL;
    }
    if (JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return JS_ATOM_NULL;
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (!js_class_has_bytecode(p->class_id))
        return JS_ATOM_NULL;
    b = p->u.func.function_bytecode;
    return JS_DupAtom(ctx, b->filename);
}

JSContext *JS_GetActiveScriptRealm(JSContext *ctx)
{
    JSStackFrame *sf;

    /* A built-in's execution context has a null ScriptOrModule, so the walk passes over every frame that is not
       running bytecode; a bytecode function's realm is the realm of the script that defined it. */
    for (sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
        JSObject *p;

        if (JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
            continue;
        p = JS_VALUE_GET_OBJ(sf->cur_func);
        if (js_class_has_bytecode(p->class_id))
            return p->u.func.function_bytecode->realm;
    }
    return NULL;
}

JSAtom JS_GetModuleName(JSContext *ctx, JSModuleDef *m)
{
    return JS_DupAtom(ctx, m->module_name);
}

JSValue JS_GetImportMeta(JSContext *ctx, JSModuleDef *m)
{
    JSValue obj;
    /* allocate meta_obj only if requested to save memory */
    obj = m->meta_obj;
    if (JS_IsUndefined(obj)) {
        /* §13.3.12.1 ImportMeta's step 4: the object, the host's properties on it, then the record's slot. The
           slot is per-timeline module state (JSModuleEvalState), announced before the write like every field
           there, so a sibling forked before this read mints its own object. */
        obj = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
        if (ctx->rt->module_import_meta_func && ctx->rt->module_import_meta_func(ctx, m, obj) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        js_module_eval_capture(ctx, m);
        m->meta_obj = obj;
    }
    return js_dup(obj);
}

static JSValue js_import_meta(JSContext *ctx)
{
    JSAtom filename;
    JSModuleDef *m;

    filename = JS_GetScriptOrModuleName(ctx, 0);
    if (filename == JS_ATOM_NULL)
        goto fail;

    /* XXX: inefficient, need to add a module or script pointer in
       JSFunctionBytecode */
    m = js_find_loaded_module(ctx, filename);
    JS_FreeAtom(ctx, filename);
    if (!m) {
    fail:
        JS_ThrowTypeError(ctx, "import.meta not supported in this context");
        return JS_EXCEPTION;
    }
    return JS_GetImportMeta(ctx, m);
}

static JSValue JS_NewModuleValue(JSContext *ctx, JSModuleDef *m)
{
    return js_dup(JS_MKPTR(JS_TAG_MODULE, m));
}

/* DELETED: js_load_module_rejected. A module load's rejection is FORWARDED UNCHANGED, so it never needed a
   reaction of its own — the whole body of that C function was `JS_Call(resolving_funcs[1], reason)`, taking the
   reason a promise had just handed it and passing the same value on. Where the import's capability is the
   PerformPromiseThen's derived one (13.3.10.3's linkAndEvaluateClosure) the forwarding is 27.5.5.4.1's
   missing-onRejected rethrow and there is nothing to register; where it is not (js_load_module_then, whose
   fulfilment reaction owes the capability nothing) the reaction is resolving_funcs[1] ITSELF.
   THE WRAPPER WAS NOT A SLOW PATH, IT WAS A DEAD END. `resolving_funcs[1]` is PAGE CODE whenever the import's
   capability came from a promise SUBCLASS, and the native one is a step machine whose settle reads
   `Get(resolution, "then")` off the value — so a loop or an await in either had no flow base under that JS_Call
   to park into and aborted. Reached as a reaction it is the callee of promise_reaction_job's CALL-ROOT FLOW, the
   same base every page-written `.catch` handler runs on, so it parks and resumes at any depth; §Every runtime
   job is explicit that a lazy chunk's `import()` is one of those flows and not a C activation.
   It also stops swallowing the reject's own throw: the wrapper freed the result and returned undefined with the
   exception still in flight, handing somebody else's failure to whatever ran next.
   Its `argc >= 1` guard and the "XXX: check if the test is necessary" beside it go with it: 27.5.2.1 calls a
   reaction handler with exactly one argument. */

/* 16.2.1.6.1.3.1's FULFILMENT continuation. `import()` resolves with the module NAMESPACE while the evaluation
   promise this is attached to fulfils with undefined, so the reaction exists only to map the one to the other —
   func_data[0] is the module record and nothing else.
   IT SETTLES BY ITS OWN COMPLETION, never by calling the capability: the import's capability is this
   PerformPromiseThen's DERIVED capability, so the namespace this RETURNS becomes phase 1 of this very flow and a
   throw becomes the import's rejection. The JS_Call it used to make was the settle done from a C activation with
   no flow base under it — and the resolving function it called is where a promise SUBCLASS's bytecode runs, and
   where the native one reads `Get(resolution, "then")`. The old error path leaked as well: it took the namespace
   failure's reason with JS_GetException and handed it to a wrapper that never freed it. */
static JSValue js_load_module_fulfilled(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic,
                                        JSValueConst *func_data)
{
    DCHECK(JS_VALUE_GET_TAG(func_data[0]) == JS_TAG_MODULE,
           "a module evaluation continuation was handed something that is not the module record it evaluated");
    return JS_GetModuleNamespace(ctx, JS_VALUE_GET_PTR(func_data[0]));
}

/* DELETED: js_load_module_reject_pending. Its whole body was `JS_Call(resolving_funcs[1], JS_GetException())` —
   the import's capability settled from a C activation, which is the identical dead end js_load_module_rejected
   was. `resolving_funcs[1]` is PAGE CODE whenever the import's capability came from a promise SUBCLASS, and the
   native one is a step machine, so a loop, an await or a getter in either had no flow base under that JS_Call to
   park into and aborted. Its "XXX: what to do if exception ?" is answered by the same move rather than by a
   policy: the settle's own throw is a COMPLETION now, never a value to drop.
   ITS FIVE CALL SITES DO NOT SHARE ONE ANSWER, because the question is what is UNDER the C activation and that
   differs. THREE OF THEM ALREADY RUN INSIDE A FLOW: js_module_source_def's machine and the two load continuations
   js_load_module_then registers are promise REACTION HANDLERS, so each is phase 0 of promise_reaction_job's
   call-root flow and its completion already has somewhere to go.
   There the rejection is the handler's OWN THROW — the exception stays in flight, the handler returns
   JS_EXCEPTION, and phase 1 of that same flow delivers it to the capability on the base the handler ran on.
   js_settle_as_flow at those three would be a SECOND flow nested inside the first, which is not a smaller
   version of the right answer but a different one: the handler's completion ALREADY reaches the capability, on
   the base the handler ran on, so a nested flow settles what phase 1 was going to settle and the reaction's own
   throw then has nowhere to go. (It used to be argued from the pump instead — "both can park and there is one
   slot" — and that argument is gone: a park is the BASE's, so nesting them queues rather than collides. The
   conclusion did not depend on it.)
   THAT ONLY WORKS BECAUSE THE REACTION'S DERIVED CAPABILITY IS NOW THE IMPORT'S REJECT — see js_load_module_then
   and JS_LoadModuleInternal, which hand perform_promise_then {undefined, resolving_funcs[1]}. js_promise_then_
   native made a THROWAWAY capability, so a handler's throw rejected a promise nobody held and the import hung.
   The resolve half stays UNDEFINED on purpose: on_loaded's normal return means "the settle is not mine, it is
   registered on the evaluation promise", and a real resolve there would settle the import with undefined the
   instant the graph loaded — resolving `import()` to undefined instead of the namespace.
   THE OTHER TWO ARE EMBEDDER ENTRIES WITH NO FLOW UNDER THEM AT ALL — JS_LoadModuleInternal (reached from
   JS_LoadModule and from js_dynamic_import_job, both plain C) and JS_EvalFunctionInternal's module arm — and
   that is exactly what js_settle_as_flow exists for: it BUILDS the call root the settle runs on. */
static void js_load_module_reject_at_entry(JSContext *ctx, JSValueConst *resolving_funcs)
{
    JSValue err;
    DCHECK(JS_HasException(ctx),
           "a module load's failure reached its entry settle with no exception in flight — the rejection reason "
           "is whatever failed, and there is nothing here to invent one from");
    err = JS_GetException(ctx);
    CHECK(js_settle_as_flow(ctx, resolving_funcs[1], err) == 0,
          "a module load's rejection could not be delivered at an embedder entry — either the flow could not be "
          "allocated or the reject threw, and there is no job pump under this call to report either to, so the "
          "import would stay pending forever");
    JS_FreeValue(ctx, err);
}

/* THE PHASE THAT COMES FIRST, declared here because both of its consumers are below it: 16.2.1.6.1.1
   LoadRequestedModules, then `on_loaded` upon its fulfilment. See js_module_load_requested.
   -1 leaves the failure's exception IN FLIGHT for the caller to deliver, because only the caller knows whether
   it is standing in a flow (return it) or at an embedder entry (js_load_module_reject_at_entry). */
static __exception int js_load_module_then(JSContext *ctx, JSModuleDef *m,
                                           JSValueConst *resolving_funcs,
                                           const JSTrampStepDef *on_loaded);

static JSValue js_module_eval_capability(JSContext *ctx, JSModuleDef *m);
static void js_promise_set_handled(JSContext *ctx, JSValueConst promise);

/* 16.2.1.6.1.3 Evaluate ( ) AS A REQUEST SUB-SEQUENCE, Link's twin below. 16.2.1.6.1.3.1 InnerModuleEvaluation
   ( module, stack, index ) runs module bodies inside its walk, so each body is a CALL request on the running
   flow's chain and the walk's cursor lives here: a park or a fork inside a body carries the rest of the walk.
   0 = Evaluate has returned (its capability settled or pending); JS_STEP_CALL / JS_STEP_YIELD = return that code
   and re-enter; -1 = threw before the capability existed. */
typedef struct JSModuleEvalOp {
    struct JSModuleEvalFrame *frames;  /* OWNED, and declared to the step teardown by the machine's `visit` */
    int depth, cap;
    int index;                         /* InnerModuleEvaluation's `index` */
    JSModuleDef *stack_top;            /* Evaluate's `stack`, threaded through JSModuleDef.stack_prev */
    JSModuleDef *root;                 /* Evaluate's `module`, after its [[CycleRoot]] step */
    JSModuleDef *body;                 /* the module whose body the CALL in flight runs, NULL when none is */
    JSValue cb[2];                     /* that CALL's operands: [this = undefined, the module's body] */
    JSValue res;                       /* its completion: the promise the async body returns */
    uint8_t phase;                     /* step_call_run's own cursor */
    uint8_t started;
} JSModuleEvalOp;

static int step_module_evaluate_run(JSContext *ctx, JSModuleEvalOp *op, JSModuleDef *m,
                                    JSValue in, JSValue **out_cb, int *out_argc);
static void step_module_evaluate_visit(JSContext *ctx, JSModuleEvalOp *op, JSStepVisit *v);

/* 16.2.1.6.1.2 Link ( ) AS A REQUEST SUB-SEQUENCE — the shared half of the two continuations that link a
   loaded graph, in the shape every other sub-sequence in this file has (step_program_run, step_call_run):
   the caller keeps the cursor in its own state, calls this at one stage, and returns whatever it answers.
     IT EXISTS BECAUSE LINKING CONTAINS A CALL. 16.2.1.7.3.1 InitializeEnvironment's declaration instantiation
   is compiled into the module body's own `OP_push_this; OP_if_false <body>` prologue, so performing it means
   ENTERING A BYTECODE BODY — and a bytecode body entered from C gets an activation with no flow base under it,
   or (which is what this replaced) one on a SECOND base built on the C stack, which the first preempt parks
   into and nobody can resume. As a CALL request the prologue's frame lands on the RUNNING flow's chain, so a
   park there is the flow's own park: the chain holds it, the pump resumes it, and this sub-sequence is
   re-entered at the module it left off at.
     0 = the graph is linked; JS_STEP_CALL / JS_STEP_YIELD = the caller must return that code and will be
   re-entered here; -1 = threw, after 16.2.1.6.1.2 step 4's reset. */
typedef struct JSModuleLinkOp {
    JSModuleLinkWalk walk;
    JSValue cb[2];        /* the prologue CALL request's operands: [this = true, the module's body] */
    JSValue res;          /* the prologue's completion — undefined by construction, and discarded */
    uint8_t phase;        /* step_call_run's own cursor */
    uint8_t started;      /* the walk has been initialised; NOT derivable from the stage (see JSTrampStepDef.steps) */
} JSModuleLinkOp;

static int step_module_link_run(JSContext *ctx, JSModuleLinkOp *op, JSModuleDef *m,
                                JSValue in, JSValue **out_cb, int *out_argc)
{
    JSModuleDef *pm;
    int r;

    if (!op->started) {
        /* EVERY OWNED FIELD BEFORE THE FIRST OPERATION THAT CAN FAIL — the teardown releases exactly what the
           machine's `visit` names, and js_mallocz leaves a JSValue reading as the INTEGER 0. */
        op->cb[0] = JS_UNDEFINED;
        op->cb[1] = JS_UNDEFINED;
        op->res = JS_UNDEFINED;
        op->phase = 0;
        op->started = 1;
        JS_FreeValue(ctx, in);
        /* must be done before the link because of cyclic references: every module of the graph needs its
           function object and its exported variable slots to exist before any of them resolves an import. */
        if (js_create_module_function(ctx, m) < 0)
            return -1;
        if (js_module_link_init(ctx, &op->walk, m) < 0)
            goto abrupt;
    } else if (op->phase != 0) {
        /* the prologue this parked on has completed: END the request where it was made, whatever it completed
           with. A rejection inside it is rethrown below, and a CALL that failed to dispatch arrives as
           JS_EXCEPTION (the machines running this sub-sequence declare catches_abrupt); both are the link's
           throw and take step 4 here. */
        r = step_call_run(ctx, &op->phase, STEP_CB(op->cb), JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                          in, &op->res, out_cb, out_argc);
        DCHECK(r == 0,
               "a module prologue's CALL request resumed and did not end — step_call_run's second phase always "
               "completes, so a step code here is a cursor that names a request nobody issued");
        if (JS_IsException(op->res)) {
            op->res = JS_UNDEFINED;
            goto abrupt;
        }
        /* The body is an async function, so the prologue's completion is its promise: an abrupt one (an
           allocation inside InstantiateFunctionObject) is the link's throw. The prologue has no await, so the
           promise is settled when the CALL returns. */
        DCHECK(JS_GetOpaque(op->res, JS_CLASS_PROMISE) != NULL
               && JS_PromiseState(ctx, op->res) != JS_PROMISE_PENDING,
               "a module prologue's CALL completed with something other than a settled promise — the prologue "
               "arm has no await, so its async activation settles before the CALL returns");
        if (JS_PromiseState(ctx, op->res) == JS_PROMISE_REJECTED) {
            js_promise_set_handled(ctx, op->res);
            JS_Throw(ctx, JS_PromiseResult(ctx, op->res));
            JS_FreeValue(ctx, op->res);
            op->res = JS_UNDEFINED;
            goto abrupt;
        }
        JS_FreeValue(ctx, op->res);
        op->res = JS_UNDEFINED;
    } else {
        JS_FreeValue(ctx, in);   /* re-entered from a JS_STEP_YIELD: the driver delivers undefined */
    }

    r = js_module_link_advance(ctx, &op->walk, &pm);
    if (r < 0)
        goto abrupt;
    if (r == MODLINK_DONE)
        return 0;
    if (r == MODLINK_PROLOGUE) {
        DCHECK(pm != NULL, "the link walk asked for a prologue and named no module to run it for");
        DCHECK(JS_VALUE_GET_TAG(pm->func_obj) == JS_TAG_OBJECT,
               "16.2.1.7.3.1: a Source Text Module Record reached its declaration instantiation with no body "
               "object — js_create_module_function builds one for every module of the graph before the link");
        /* `this === true` is what selects the prologue: the compiler emits `OP_push_this; OP_if_false <body>`
           at the head of a module body, so the true arm performs the declaration instantiation and returns
           while the false arm is the module's own code, which 16.2.1.6.1.3 Evaluate ( ) enters later. */
        r = step_call_run(ctx, &op->phase, STEP_CB(op->cb), pm->func_obj, JS_TRUE, 0, NULL,
                          JS_UNDEFINED, &op->res, out_cb, out_argc);
        DCHECK(r == JS_STEP_CALL,
               "a module prologue's CALL request answered something other than a call — its cursor was already "
               "in flight, so the answer to the OLD request is about to be collected as this one's");
        return r;
    }
    DCHECK(r == MODLINK_MORE, "js_module_link_advance answered a code it does not define");
    /* A REST WITH NOTHING TO RUN, and it is offered rather than skipped: the graph is the PAGE'S SIZE, so a
       walk that never yields runs to completion inside one opcode however flat its frames are. */
    return JS_STEP_YIELD;
 abrupt:
    js_module_link_abrupt(ctx, &op->walk, m);
    return -1;
}

static void step_module_link_visit(JSContext *ctx, JSModuleLinkOp *op, JSStepVisit *v)
{
    int k;
    for (k = 0; k < 2; k++)
        v->val(ctx, &op->cb[k]);
    v->val(ctx, &op->res);
    v->buf(ctx, (void **)&op->walk.frames, (size_t)op->walk.cap * sizeof(*op->walk.frames));
}

/* A fork may clone this machine at any rest of either walk. The clone copies each cursor (`visit` declares the
   frame arrays), every record field the walks write is per-flow state the arm's delta carries
   (JSModuleEvalState), and the import cells js_module_linking_bindings wires are shared cells it resolves
   idempotently — so each arm finishes the link in its own world, exactly as a sibling forked before the link
   links again. */
typedef struct JSModuleLoaded {
    JSStepHdr hdr;          /* MUST be first. The closure's data is [resolve, reject, module]. */
    JSModuleLinkOp op;      /* 16.2.1.6.1.2 Link ( ) */
    JSModuleEvalOp ev;      /* 16.2.1.6.1.3 Evaluate ( ) */
    JSValue cap;            /* 16.2.1.6.1.3 Evaluate ( )'s promise, held across the settle that adopts it */
    JSValue settle_cb[3];   /* the load's own resolve CALL: [this, resolve, cap] */
    JSValue settle_res;
    uint8_t settle_phase;
} JSModuleLoaded;
_Static_assert(offsetof(JSModuleLoaded, hdr) == 0, "JSStepHdr must be first in JSModuleLoaded");

static void js_module_loaded_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSModuleLoaded *s = st;
    int k;
    step_module_link_visit(ctx, &s->op, v);
    step_module_evaluate_visit(ctx, &s->ev, v);
    v->val(ctx, &s->cap);
    v->val(ctx, &s->settle_res);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->settle_cb[k]);
}

/* 13.3.10.3 ContinueDynamicImport step 8's linkAndEvaluateClosure, run once the graph is LOADED (step 12's
   PerformPromiseThen puts it there): Link, take the evaluation promise, register step 8.e's onFulfilled on it,
   and Evaluate. The closure's data is [resolve, reject, module] — the shape js_load_module_then builds.
   A LINK OR CAPABILITY FAILURE IS THIS MACHINE'S OWN THROW, delivered to the import's reject as phase 1 of the
   flow it runs on, which is what step 8.b's `Call(promiseCapability.[[Reject]], …)` is here. The NORMAL
   completion is undefined and settles nothing: the import's settlement belongs to the evaluation promise this
   just attached to, not to the graph having loaded. */
#define MODIMP_STAGES(X) \
    X(MODIMP_LINK,     "13.3.10.3 step 8.a (link is Completion(module.Link())) — 16.2.1.6.1.2 step 3's " \
                       "InnerModuleLinking, resting once per module of the graph because each one's " \
                       "16.2.1.7.3.1 declaration instantiation is a CALL of that module's own body") \
    X(MODIMP_EVALUATE, "13.3.10.3 steps 8.c-8.f (evaluatePromise is module.Evaluate(); " \
                       "PerformPromiseThen(evaluatePromise, onFulfilled, onRejected))")
enum { MODIMP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_module_loaded_import_steps[] = { MODIMP_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_module_loaded_import_step(JSContext *ctx, void *st, JSValue cb_result,
                                        JSValue **out_cb, int *out_argc)
{
    JSModuleLoaded *s = st;
    JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
    JSModuleDef *m;
    JSValue cap, func_obj, evaluate_resolving_funcs[2];
    int r;

    DCHECK(JS_VALUE_GET_TAG(rec->data[2]) == JS_TAG_MODULE,
           "a load continuation was handed something that is not the module record it loaded");
    m = JS_VALUE_GET_PTR(rec->data[2]);

    STEP_DISPATCH(MODIMP_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(MODIMP_LINK);
        if (!s->op.started) {
            s->cap = JS_UNDEFINED;
            s->settle_res = JS_UNDEFINED;
            s->settle_cb[0] = s->settle_cb[1] = s->settle_cb[2] = JS_UNDEFINED;
            s->settle_phase = 0;
            s->ev.started = 0;
            s->ev.frames = NULL;
            s->ev.cb[0] = s->ev.cb[1] = JS_UNDEFINED;
            s->ev.res = JS_UNDEFINED;
        }
        r = step_module_link_run(ctx, &s->op, m, cb_result, out_cb, out_argc);
        if (r)
            return r < 0 ? JS_STEP_ABRUPT : r;
        STEP_GOTO(s->hdr.stage, MODIMP_EVALUATE, &s->op.phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(MODIMP_EVALUATE);
        if (s->ev.started)
            goto evaluate;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* THE CAPABILITY BEFORE EVALUATING, and step 8.f's PerformPromiseThen before it too — spelled as the
           three operations rather than as JS_EvalFunction precisely so the registration lands between the
           second and the third. The module body can FORK; a sibling forked mid-body inherits the world as it
           was at the branch, and this continuation has to already be on the capability in that world or the
           importing `.then(m => …)` never runs there. */
        cap = js_module_eval_capability(ctx, m);
        if (JS_IsException(cap))
            return JS_STEP_ABRUPT;

        /* THE IMPORT'S CAPABILITY IS THE DERIVED CAPABILITY OF THIS PerformPromiseThen, which is what stops
           either reaction from having to CALL it. js_promise_then_native made a throwaway capability instead,
           so both reactions settled the import by JS_Call — and that is the whole reason this pair had a
           rejection wrapper and a resolve call in it. Handed the real capability, step 8.e's onFulfilled
           settles by RETURNING (phase 1 of its own flow) and step 4's onRejected needs no reaction at all:
           27.5.5.4.1 PerformPromiseThen leaves a missing onRejected's handler EMPTY and 27.5.2.1
           NewPromiseReactionJob rethrows the reason, and perform_promise_then's no-handler path forwards
           the reason to rec->data[1] through js_settle_as_flow — a call root a subclass's reject can park
           inside. func_data is therefore the MODULE ALONE: the reaction stopped holding the capability when it
           stopped calling it. */
        func_obj = JS_NewModuleValue(ctx, m);
        evaluate_resolving_funcs[0] = JS_NewCFunctionData(ctx, js_load_module_fulfilled, 0, 0, 1, vc(&func_obj));
        evaluate_resolving_funcs[1] = JS_UNDEFINED;
        CHECK(!JS_IsException(evaluate_resolving_funcs[0]),
              "a module evaluation could not build its continuation — the module would evaluate and reach nobody");
        JS_FreeValue(ctx, func_obj);
        CHECK(perform_promise_then(ctx, cap, vc(evaluate_resolving_funcs), vc(rec->data)) == 0,
              "a module evaluation's continuation could not be attached — the import would never settle");
        JS_FreeValue(ctx, evaluate_resolving_funcs[0]);
        JS_FreeValue(ctx, cap);
    evaluate:
        /* The capability exists, so the only failure Evaluate can still report is the one that creates it;
           every evaluation error settles that capability instead, which is what the reactions above are for. */
        r = step_module_evaluate_run(ctx, &s->ev, m, cb_result, out_cb, out_argc);
        DCHECK(r >= 0, "module evaluation threw after its capability existed — an evaluation error settles the "
                       "capability, it does not throw past it");
        if (r)
            return r < 0 ? JS_STEP_ABRUPT : r;
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_module_loaded_import_def = {
    sizeof(JSModuleLoaded), js_module_loaded_import_step, NULL, 0,
    .catches_abrupt = 1,   /* a CALL that fails to dispatch is the link's or the evaluation's throw */
    .visit = js_module_loaded_visit,
    .algorithm = "13.3.10.3 ContinueDynamicImport step 8 linkAndEvaluateClosure",
    .steps = js_module_loaded_import_steps
};

/* A MODULE SOURCE'S COMPILE, held across the rests its parse takes: the UTF-8 the parse borrows and the parked
   parse. A cache over the reaction's own source argument and name, so a fork sheds it and the sibling parses
   again from them. */
typedef struct {
    JSEvalCompile *ec;
    const char *src, *name;
    size_t len;
} JSModSrcCompile;

static void js_modsrc_compile_destroy(JSContext *ctx, void *p)
{
    JSModSrcCompile *c = p;

    js_eval_compile_drop(ctx, &c->ec);
    JS_FreeCString(ctx, c->src);
    JS_FreeCString(ctx, c->name);
    js_free(ctx, c);
}

/* Compile `src_v` as the module named `name_v`, one stint per entry. ALREADY LOADED is asked before every stint:
   two flows exploring one page can park on one specifier and both resume with its body, and the module map is
   the load's memo (16.2.1.11 finishes a load another completed by handing back the existing record), so the
   record that is published first wins and a parked parse of the same name is dropped. The final stint publishes
   inside the stint, so nothing interleaves between that check and the publication.
   0 with *pm the record, JS_STEP_YIELD while the parse is parked in *pcomp, -1 with the exception pending. */
static int step_module_source_compile(JSContext *ctx, void **pcomp, JSValueConst src_v, JSValueConst name_v,
                                      JSModuleDef **pm)
{
    JSModSrcCompile *c = *pcomp;
    JSAtom name_atom = JS_ValueToAtom(ctx, name_v);
    JSModuleDef *m;
    JSValue func_obj;

    if (name_atom == JS_ATOM_NULL)
        return -1;
    m = js_find_loaded_module(ctx, name_atom);
    JS_FreeAtom(ctx, name_atom);
    if (m) {
        if (c) {
            js_modsrc_compile_destroy(ctx, c);
            *pcomp = NULL;
        }
        *pm = m;
        return 0;
    }
    if (!c) {
        JSEvalCompile **outer = g_eval_compile_slot;

        c = js_mallocz(ctx, sizeof *c);
        if (!c)
            return -1;
        c->src = JS_ToCStringLen(ctx, &c->len, src_v);
        c->name = JS_ToCString(ctx, name_v);
        *pcomp = c;
        if (!c->src || !c->name) {
            js_modsrc_compile_destroy(ctx, c);
            *pcomp = NULL;
            return -1;
        }
        g_eval_compile_slot = &c->ec;
        func_obj = JS_Eval(ctx, c->src, c->len, c->name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        g_eval_compile_slot = outer;
    } else {
        DCHECK(c->ec != NULL, "a module source compile resumed with nothing parked");
        func_obj = js_eval_compile_continue(ctx, &c->ec);
    }
    if (JS_VALUE_GET_TAG(func_obj) == JS_TAG_UNINITIALIZED)
        return JS_STEP_YIELD;
    DCHECK(c->ec == NULL, "a module source compile answered and left a parked parse behind");
    js_modsrc_compile_destroy(ctx, c);
    *pcomp = NULL;
    if (JS_IsException(func_obj))
        return -1;
    DCHECK(JS_VALUE_GET_TAG(func_obj) == JS_TAG_MODULE, "compiling a module source did not produce a module record");
    m = JS_VALUE_GET_PTR(func_obj);
    JS_FreeValue(ctx, func_obj);   /* the module map owns the record */
    *pm = m;
    return 0;
}

/* The source arrived. func_data = [resolve, reject, module_name]; argv[0] is the module's SOURCE TEXT. Compiling
   it registers the record under that name, so a second import of the same specifier finds it loaded and never
   asks the host again. This is the resume of the load the loader parked — the importing flow's continuation is
   the reaction chain this runs on, never a re-run of the importing scope.
   EVERY FAILURE HERE IS THIS REACTION'S OWN THROW, delivered to the import's reject as phase 1 of this flow. */
typedef struct JSModSrc {
    JSStepHdr hdr;
    void *comp;   /* the parse in flight (JSModSrcCompile, a cache) */
} JSModSrc;
#define MODSRC_STAGES(X) \
    X(MODSRC_COMPILE, "13.3.10.3 ContinueDynamicImport, the host's 16.2.1.10 HostLoadImportedModule finishing: " \
                      "the module record is created from the fetched source text (16.2.1.7.1 ParseModule)")
enum { MODSRC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_module_source_steps[] = { MODSRC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_module_source_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSModSrc *s = st;
    JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
    JSModuleDef *m = NULL;
    int r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == MODSRC_COMPILE, "a module source reaction resumed in no stage");
    r = step_module_source_compile(ctx, &s->comp, step_arg(&s->hdr, 0), rec->data[2], &m);
    if (r)
        return r < 0 ? JS_STEP_ABRUPT : r;
    /* COMPILING IS NOT LOADING. The record has its own `import`s and an EMPTY [[LoadedModules]] (16.2.1.7.1), so
       what follows the compile is the load of ITS graph — not the link. */
    if (js_load_module_then(ctx, m, rec->data, &js_module_loaded_import_def) < 0)
        return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static void js_module_source_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSModSrc *s = st;
    v->cache(ctx, &s->comp, js_modsrc_compile_destroy);
}

static JSValue js_module_source_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx; (void)st; (void)take_result;
    return JS_UNDEFINED;
}

static const JSTrampStepDef js_module_source_def = {
    sizeof(JSModSrc), js_module_source_step, js_module_source_fini, 0,
    .visit = js_module_source_visit,
    .algorithm = "13.3.10.3 ContinueDynamicImport, the module source's arrival",
    .steps = js_module_source_steps
};

/* The source fetch itself failed: the import rejects with whatever the host settled the source promise with,
   which is what a browser does for a module whose network request did not succeed. */
static JSValue js_module_source_rejected(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic,
                                         JSValueConst *func_data)
{
    JSValue ret = js_promise_settle_native(ctx, func_data[1], argv);
    JS_FreeValue(ctx, ret);
    return JS_UNDEFINED;
}

void JS_ModuleLoadPending(JSContext *ctx, JSValue source_promise)
{
    DCHECK(JS_IsUndefined(ctx->module_load_pending),
           "two module loads parked on one handoff slot — a loader parked without its caller taking the first");
    ctx->module_load_pending = source_promise;
}

/* Did the loader just park? Consumes the handoff slot: JS_UNDEFINED means it answered synchronously (with a
   module, or with a throw). Every caller of js_host_resolve_imported_module asks, because a caller that cannot
   wait must say so rather than silently drop the load.
   THE NAME COMES WITH IT, always, because half a handoff is the shape a consumer papers over with a default:
   the source text does not say what the module is CALLED, and the caller that compiles it has only the raw
   specifier it typed, which is not the resolved one. Both slots are written together by
   js_host_resolve_imported_module and both are cleared here, and the DCHECK is that they never split. */
static JSValue js_module_take_pending(JSContext *ctx, JSValue *pname)
{
    JSValue p = ctx->module_load_pending;

    DCHECK(pname != NULL, "a parked module load was taken without its resolved specifier — the name is not "
                          "optional, it is the module map key and the base its own imports resolve against");
    *pname = ctx->module_load_pending_name;
    ctx->module_load_pending = JS_UNDEFINED;
    ctx->module_load_pending_name = JS_UNDEFINED;
    DCHECK(JS_IsUndefined(p) == JS_IsUndefined(*pname),
           "a parked module load has a source promise without a resolved specifier, or a specifier without a "
           "source promise — the two are written by one line and must arrive together");
    return p;
}

/* ============ 16.2.1.6.1.1 LoadRequestedModules — THE LOAD IS A PHASE, AND IT IS NOT THE LINK ============
 *
 * ECMAScript gives a module graph three operations in this order, and the order is the whole design:
 *
 *   LoadRequestedModules  populates every record's [[LoadedModules]] and returns a PROMISE. It is the only one
 *                         of the three that talks to the host (16.2.1.10 HostLoadImportedModule), and the host
 *                         may answer whenever it likes — 16.2.1.11 FinishLoadingImportedModule is called
 *                         "either synchronously or asynchronously".
 *   Link                  walks the graph that phase filled in. It reaches every child through 16.2.1.9
 *                         GetImportedModule, whose entire body is an ASSERTION that the record is already
 *                         there. Link never loads, so link never waits.
 *   Evaluate              runs it.
 *
 * HTML says the same thing in its own words: "fetch a single module script" calls record.LoadRequestedModules(),
 * and only "upon fulfillment of loadingPromise" does it "Perform record.Link()".
 *
 * quickjs conflated the first two into js_resolve_module — a synchronous C walk that both descended the graph
 * and asked the host for every child it did not have, run at COMPILE time as well as at link time. A host that
 * cannot answer on the spot had nowhere to park inside it, which is exactly what a browser's module fetch is,
 * and what SECURITY.md forces this engine's to be (every byte of network goes through safeFetch). A static
 * `import` therefore hit a DFAIL saying precisely that, and this is that DFAIL built.
 *
 * THE WALK IS RE-ENTERED, NOT RESUMED, and that is the spec's shape rather than a simplification of it.
 * InnerModuleLoading issues a host request and KEEPS GOING through the rest of the referrer's requests; when an
 * answer arrives later, ContinueModuleLoading starts a fresh InnerModuleLoading rooted at the module that
 * arrived. So there is no cursor to park and no half-finished traversal to serialise — what carries across the
 * park is the GraphLoadingState and the [[LoadedModules]] links already written into the records, and both of
 * those are ordinary heap state the flow's COW delta and its snapshot already cover. A partially loaded graph
 * is a set of records in the module map plus a pending count; nothing about it is a C stack.
 *
 * WHAT IS STILL C RECURSION HERE: nothing. The already-loaded arm of InnerModuleLoading is spec'd as a
 * recursive call and its depth is the PAGE'S graph depth, so it is an explicit frame stack for the same reason
 * js_inner_module_linking is one.
 */

/* The GraphLoadingState Record of 16.2.1.6.1.1, as a JS OBJECT and not a malloc'd C record — because a load
   PARKS, which makes this data a flow QUEUES. It must fork per flow (two arms of a branch that both import are
   two loads, and one arm's [[PendingModulesCount]] is not the other's), it must park to the cold tier with the
   flow and come back, and it must be reachable by the collector while the only thing holding it is a promise
   reaction's func_data. A malloc'd record named by a raw pointer from a JSCFunctionData does none of the three,
   and the leak it makes is one the runtime's own gc_obj_list walk cannot see.
   Its prototype is NULL: a page that writes Object.prototype.pending must not intercept the engine's own
   bookkeeping, and a setter there would run the page's code inside the load. */
static JSValue js_gls_new(JSContext *ctx, JSValueConst *resolving_funcs)
{
    JSValue st, visited;

    st = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(st))
        return JS_EXCEPTION;
    visited = JS_NewArray(ctx);
    if (JS_IsException(visited)) {
        JS_FreeValue(ctx, st);
        return JS_EXCEPTION;
    }
    /* [[PendingModulesCount]] starts at 1: the root InnerModuleLoading call is itself one completion to
       account for, which is what stops an empty graph resolving before it has been walked. */
    if (JS_DefinePropertyValueStr(ctx, st, "visited", visited, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, st, "loading", js_bool(true), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, st, "pending", js_int32(1), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, st, "resolve", js_dup(resolving_funcs[0]), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, st, "reject", js_dup(resolving_funcs[1]), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, st);
        return JS_EXCEPTION;
    }
    return st;
}

/* Every read below DCHECKs the field's TAG rather than defaulting it. A GraphLoadingState field has exactly one
   writer, so a read that finds undefined is a producer that never wrote — the failure a `|| 0` would turn into
   a plausible count. */
static bool js_gls_is_loading(JSContext *ctx, JSValueConst state)
{
    JSValue v = JS_GetPropertyStr(ctx, state, "loading");
    bool b;

    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_BOOL,
           "a module load's [[IsLoading]] is not the boolean its only writer writes");
    b = (JS_VALUE_GET_BOOL(v) != 0);
    JS_FreeValue(ctx, v);
    return b;
}

static void js_gls_set_loading(JSContext *ctx, JSValueConst state, bool b)
{
    CHECK(JS_SetPropertyStr(ctx, state, "loading", js_bool(b)) >= 0,
          "a module load's [[IsLoading]] could not be written — a load that cannot record its own failure "
          "goes on issuing requests after it has rejected");
}

static int js_gls_pending(JSContext *ctx, JSValueConst state)
{
    JSValue v = JS_GetPropertyStr(ctx, state, "pending");
    int n;

    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a module load's [[PendingModulesCount]] is not the integer its only writer writes");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(ctx, v);
    return n;
}

static void js_gls_set_pending(JSContext *ctx, JSValueConst state, int n)
{
    CHECK(JS_SetPropertyStr(ctx, state, "pending", js_int32(n)) >= 0,
          "a module load's [[PendingModulesCount]] could not be written — the load would never finish");
}

/* 16.2.1.6.1.1.1 step 2.a's `Append module to state.[[Visited]]`, answering step 2's `state.[[Visited]] does
   not contain module` on the way. Returns true when this call is the one that appended it.
   [[Visited]] IS NOT m->loaded, and collapsing them is a real defect rather than a tidy-up: `[[Status]] is new`
   is a fact about the RECORD that survives every load, while [[Visited]] is a fact about THIS load. A load that
   fails leaves every record it touched still `new` (step 5.b runs only when the count reaches zero), so a later
   load walks that graph again; a single flag set on the way in would have marked those records loaded and left
   their unanswered requests to reach Link. */
static bool js_gls_visit(JSContext *ctx, JSValueConst state, JSModuleDef *m)
{
    JSValue arr, el;
    uint32_t i, len = 0;
    bool fresh = true;

    arr = JS_GetPropertyStr(ctx, state, "visited");
    DCHECK(JS_IsArray(arr), "a module load's [[Visited]] is not the array its only writer writes");
    CHECK(js_get_length32(ctx, &len, arr) == 0, "a module load's [[Visited]] length could not be read");
    for (i = 0; i < len; i++) {
        el = JS_GetPropertyUint32(ctx, arr, i);
        DCHECK(JS_VALUE_GET_TAG(el) == JS_TAG_MODULE,
               "a module load's [[Visited]] holds something that is not a module record");
        if (JS_VALUE_GET_PTR(el) == m)
            fresh = false;
        JS_FreeValue(ctx, el);
        if (!fresh)
            break;
    }
    if (fresh)
        CHECK(JS_SetPropertyUint32(ctx, arr, len, JS_NewModuleValue(ctx, m)) >= 0,
              "a module could not be appended to a load's [[Visited]] — a dropped visit re-walks a cycle "
              "forever and double-counts every request in it");
    JS_FreeValue(ctx, arr);
    return fresh;
}

/* 16.2.1.6.1.1.1 step 5.b: `If loaded.[[Status]] is new, set loaded.[[Status]] to unlinked`, for the whole
   [[Visited]] list AT ONCE and only when the load succeeded. */
static void js_gls_mark_loaded(JSContext *ctx, JSValueConst state)
{
    JSValue arr, el;
    uint32_t i, len = 0;

    arr = JS_GetPropertyStr(ctx, state, "visited");
    DCHECK(JS_IsArray(arr), "a module load's [[Visited]] is not the array its only writer writes");
    CHECK(js_get_length32(ctx, &len, arr) == 0, "a module load's [[Visited]] length could not be read");
    for (i = 0; i < len; i++) {
        el = JS_GetPropertyUint32(ctx, arr, i);
        DCHECK(JS_VALUE_GET_TAG(el) == JS_TAG_MODULE,
               "a module load's [[Visited]] holds something that is not a module record");
        ((JSModuleDef *)JS_VALUE_GET_PTR(el))->loaded = true;
        JS_FreeValue(ctx, el);
    }
    JS_FreeValue(ctx, arr);
}

/* 16.2.1.6.1.1.1 steps 3-5: the TAIL OF EVERY InnerModuleLoading CALL, whether that call walked a module or
   found nothing to do. The load finishes when the last outstanding call accounts for itself. */
static void js_module_loading_settle_one(JSContext *ctx, JSValueConst state)
{
    int n = js_gls_pending(ctx, state);

    DCHECK(n >= 1, "16.2.1.6.1.1.1 step 4: a module load accounted for more completions than it requested");
    n--;
    js_gls_set_pending(ctx, state, n);
    if (n == 0) {
        JSValue res, ret, arg = JS_UNDEFINED;
        js_gls_set_loading(ctx, state, false);
        js_gls_mark_loaded(ctx, state);
        res = JS_GetPropertyStr(ctx, state, "resolve");
        DCHECK(JS_IsFunction(ctx, res), "a module load's capability resolve is not a function");
        ret = js_promise_settle_native(ctx, res, vc(&arg));
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, res);
    }
}

static void js_inner_module_loading(JSContext *ctx, JSValueConst state, JSModuleDef *m);

/* 16.2.1.6.1.1.2 ContinueModuleLoading for a throw completion, whose exception is in flight. It is its own
   function because InnerModuleLoading's host load reaches it, and the normal completion's arm is the one that
   walks the graph: kept apart, a failing request never re-enters the walk that issued it. */
static void js_module_loading_fail(JSContext *ctx, JSValueConst state)
{
    JSValue rej, err, ret;

    if (!js_gls_is_loading(ctx, state)) {
        /* step 1: the load already settled, so this completion is DISCARDED — a graph's requests overlap, so a
           second one failing after the first has already rejected is ordinary. A discarded THROW completion
           still has its exception in flight, and leaving it there hands somebody else's failure to whatever
           runs next out of this job. */
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    DCHECK(JS_HasException(ctx),
           "a module load's throw completion arrived with no exception in flight — the rejection reason is "
           "whatever failed, and there is nothing here to invent one from");
    js_gls_set_loading(ctx, state, false);
    err = JS_GetException(ctx);
    rej = JS_GetPropertyStr(ctx, state, "reject");
    DCHECK(JS_IsFunction(ctx, rej), "a module load's capability reject is not a function");
    ret = js_promise_settle_native(ctx, rej, vc(&err));
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, rej);
}

/* 16.2.1.6.1.1.2 ContinueModuleLoading — the RE-ENTRY POINT, and the reason the walk needs no parked cursor: a
   request answered later restarts the loading process AT THE MODULE THAT ARRIVED, not at the position some
   traversal had reached. `m == NULL` is the throw completion (js_module_loading_fail). */
static void js_continue_module_loading(JSContext *ctx, JSValueConst state, JSModuleDef *m)
{
    if (!m) {
        js_module_loading_fail(ctx, state);
        return;
    }
    if (!js_gls_is_loading(ctx, state))
        return;                          /* step 1: the load already settled, so this completion is discarded */
    js_inner_module_loading(ctx, state, m);
}

/* THE RE-ENTRY ITSELF. func_data = [state, referrer, request index, resolved specifier]; argv[0] is the module's
   SOURCE TEXT, which is what the loader parked on. Compiling it registers the record under the resolved name,
   so a second import of the same specifier finds it in the map and never asks the host again. This is the
   RESUME of the load: the importing flow's continuation is the reaction chain this runs on, and no scope is
   ever re-entered (a re-run would be a replay). The compile rests like the dynamic import's
   (step_module_source_compile). */
#define MODREQ_STAGES(X) \
    X(MODREQ_COMPILE, "16.2.1.11 FinishLoadingImportedModule, the host's 16.2.1.10 HostLoadImportedModule " \
                      "finishing a static request: the record is created from the fetched source text " \
                      "(16.2.1.7.1 ParseModule)")
enum { MODREQ_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_module_request_steps[] = { MODREQ_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_module_request_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSModSrc *s = st;
    JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
    JSValueConst state = rec->data[0];
    JSModuleDef *referrer, *m = NULL;
    JSReqModuleEntry *rme;
    int idx, r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == MODREQ_COMPILE, "a module request reaction resumed in no stage");
    DCHECK(s->hdr.argc >= 1, "a module source promise fulfilled with no value — what the loader parked on is the "
                             "module's SOURCE TEXT, and there is nothing to compile without it");
    DCHECK(JS_VALUE_GET_TAG(rec->data[1]) == JS_TAG_MODULE,
           "a parked module request's referrer is not a module record");
    DCHECK(JS_VALUE_GET_TAG(rec->data[2]) == JS_TAG_INT,
           "a parked module request's index is not the integer its only writer writes");
    referrer = JS_VALUE_GET_PTR(rec->data[1]);
    idx = JS_VALUE_GET_INT(rec->data[2]);
    DCHECK(idx >= 0 && idx < referrer->req_module_entries_count,
           "a parked module request names an index outside its referrer's request table");
    rme = &referrer->req_module_entries[idx];

    /* 16.2.1.6.1.1.2 step 1: a sibling request already rejected this load, before or while the parse rested. */
    if (!js_gls_is_loading(ctx, state)) {
        if (s->comp) {
            js_modsrc_compile_destroy(ctx, s->comp);
            s->comp = NULL;
        }
        return JS_STEP_DONE;
    }
    r = step_module_source_compile(ctx, &s->comp, step_arg(&s->hdr, 0), rec->data[3], &m);
    if (r > 0)
        return r;
    if (r < 0) {
        js_continue_module_loading(ctx, state, NULL);
        return JS_STEP_DONE;
    }
    /* 16.2.1.11 step 1.a: a request already answered must be answered with the SAME record. 16.2.1.10 requires
       the host to be idempotent per (referrer, request), and a host that is not would silently give one import
       statement two module records. */
    DCHECK(rme->module == NULL || rme->module == m,
           "16.2.1.10's idempotence broken: one (referrer, request) was answered with two different records");
    rme->module = m;                     /* step 1.b: append to referrer's [[LoadedModules]] */
    js_continue_module_loading(ctx, state, m);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_module_request_def = {
    sizeof(JSModSrc), js_module_request_step, js_module_source_fini, 0,
    .visit = js_module_source_visit,
    .algorithm = "16.2.1.11 FinishLoadingImportedModule, a static request's module source arrival",
    .steps = js_module_request_steps
};

/* The source fetch itself failed: 16.2.1.11 with a throw completion, which ContinueModuleLoading turns into the
   load's rejection — what a browser does for a module whose network request did not succeed. */
static JSValue js_module_request_rejected(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv, int magic,
                                          JSValueConst *func_data)
{
    DCHECK(argc >= 1, "a module source promise rejected with no reason");
    JS_Throw(ctx, js_dup(argv[0]));
    js_continue_module_loading(ctx, func_data[0], NULL);
    return JS_UNDEFINED;
}

/* 16.2.1.10 HostLoadImportedModule for ONE request of `referrer`, plus 16.2.1.11 FinishLoadingImportedModule
   when the host answers on the spot. Returns the record to descend into, or NULL — either because the host
   PARKED (the re-entry is now a reaction on the source promise) or because it threw (which stops the load). */
static JSModuleDef *js_module_request_host_load(JSContext *ctx, JSValueConst state,
                                                JSModuleDef *referrer, int idx)
{
    JSReqModuleEntry *rme = &referrer->req_module_entries[idx];
    JSModuleDef *m1;
    JSValue pending, name, mv, reactions[2];
    JSValueConst func_data[4];

    m1 = js_host_resolve_imported_module_atom(ctx, referrer->module_name,
                                              rme->module_name, rme->attributes);
    if (m1) {
        rme->module = m1;                /* 16.2.1.11 step 1.b, answered synchronously */
        return m1;
    }
    pending = js_module_take_pending(ctx, &name);
    if (JS_IsUndefined(pending)) {
        js_module_loading_fail(ctx, state);
        return NULL;
    }
    /* WHAT THE RE-ENTRY NEEDS, AND ONLY THAT: the state to continue, the referrer and the INDEX of the request
       inside it (a request table is fixed once its module is compiled, so the index still names the same entry
       however long the fetch takes), and the resolved specifier to compile under. Everything else about where
       the walk had got to is deliberately absent — ContinueModuleLoading does not resume a traversal. */
    mv = JS_NewModuleValue(ctx, referrer);
    func_data[0] = state;
    func_data[1] = mv;
    func_data[2] = js_int32(idx);
    func_data[3] = name;
    reactions[0] = js_new_step_closure(ctx, &js_module_request_def, 1, 0, 4, func_data);
    reactions[1] = JS_NewCFunctionData(ctx, js_module_request_rejected, 1, 0, 4, func_data);
    CHECK(!JS_IsException(reactions[0]) && !JS_IsException(reactions[1]),
          "a parked module request could not build its re-entry — the load would never resume and the flow "
          "that imported would stay parked on a source that does arrive and reaches nobody");
    JS_FreeValue(ctx, mv);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, js_promise_then_native(ctx, pending, vc(reactions)));
    JS_FreeValue(ctx, reactions[0]);
    JS_FreeValue(ctx, reactions[1]);
    JS_FreeValue(ctx, pending);
    return NULL;
}

typedef struct JSModuleLoadFrame {
    JSModuleDef *m;
    int i;                               /* cursor over m->req_module_entries */
} JSModuleLoadFrame;

/* 16.2.1.6.1.1.1 InnerModuleLoading, as an explicit frame stack. The spec's recursion is into a module the
   referrer ALREADY has a record for, and its depth is the page's graph depth, so recursing in C would make a
   deep import chain a C-stack limit — the same reason js_inner_module_linking is a frame stack. Nothing here
   caps the walk.
   ORDER IS OBSERVABLE: the host is asked for each request in source order, and a child already in
   [[LoadedModules]] is descended into immediately, before the referrer's next request. That is where the
   recursive call sits in the spec, and this engine's loader RECORDS every specifier it is asked for, so the
   order is part of what the tool reports. */
static void js_inner_module_loading(JSContext *ctx, JSValueConst state, JSModuleDef *m)
{
    JSModuleLoadFrame *frames = NULL;
    int depth = 0, cap = 0;

    DCHECK(js_gls_is_loading(ctx, state),
           "16.2.1.6.1.1.1 step 1: InnerModuleLoading ran for a load that is no longer loading");
    for (;;) {
#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
        if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
            char buf1[ATOM_GET_STR_BUF_SIZE];
            printf("loading module '%s':\n", JS_AtomGetStr(ctx, buf1, sizeof(buf1), m->module_name));
        }
#endif
        /* ENTER `m`. A record whose [[Status]] is not `new`, or one this load has already visited, is not
           walked — but the CALL still ends, and its end is the decrement below. */
        if (m->loaded || !js_gls_visit(ctx, state, m)) {
            js_module_loading_settle_one(ctx, state);
        } else {
            /* step 2.c: every request will produce exactly one further InnerModuleLoading call, now or when
               the host answers, and each of those ends in one decrement. */
            js_gls_set_pending(ctx, state,
                               js_gls_pending(ctx, state) + m->req_module_entries_count);
            if (depth == cap) {
                int ncap = cap ? cap * 2 : 8;
                JSModuleLoadFrame *nf = js_realloc(ctx, frames, sizeof(*nf) * (size_t)ncap);
                CHECK(nf != NULL, "a module graph load could not grow its walk — a dropped frame silently "
                                  "leaves part of the graph unloaded and Link then asserts on it");
                frames = nf;
                cap = ncap;
            }
            frames[depth].m = m;
            frames[depth].i = 0;
            depth++;
        }
        for (;;) {
            JSModuleLoadFrame *f;
            JSReqModuleEntry *rme;
            JSModuleDef *m1;

            if (depth == 0 || !js_gls_is_loading(ctx, state))
                goto done;
            f = &frames[depth - 1];
            if (f->i >= f->m->req_module_entries_count) {
                depth--;                 /* this module's own call ends here */
                js_module_loading_settle_one(ctx, state);
                continue;
            }
            rme = &f->m->req_module_entries[f->i];
            f->i++;
            if (rme->module) {           /* step 2.d.ii: already in [[LoadedModules]] */
                m = rme->module;
                break;                   /* descend */
            }
            m1 = js_module_request_host_load(ctx, state, f->m, f->i - 1);
            if (m1) {
                m = m1;
                break;                   /* descend into what the host answered with */
            }
            /* PARKED, or the request threw. Neither leaves anything to descend into now, and neither stops the
               referrer's remaining requests from being issued — which is why a graph's fetches overlap. */
        }
    }
 done:
    js_free(ctx, frames);
}

/* 16.2.1.6.1.1 LoadRequestedModules. Returns the load's promise. */
static JSValue js_module_load_requested(JSContext *ctx, JSModuleDef *m)
{
    JSValue promise, resolving_funcs[2], state;

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;
    state = js_gls_new(ctx, vc(resolving_funcs));
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    if (JS_IsException(state)) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    js_inner_module_loading(ctx, state, m);
    JS_FreeValue(ctx, state);
    return promise;
}

/* Load the graph, and upon fulfilment run `on_loaded` — the shape both consumers share. `on_loaded` receives
   func_data = [resolve, reject, module].
   THE DERIVED CAPABILITY IS {UNDEFINED, resolving_funcs[1]}, and each half is a statement.
     reject   — a rejected load rejects the caller's capability with the SAME reason, and 27.5.5.4.1 says a
                missing onRejected RETHROWS, so there is no reaction to register: promise_reaction_job's
                no-handler path forwards the reason through js_settle_as_flow, a call root a subclass's reject
                can park inside. It also carries `on_loaded`'s OWN throw, which is what lets every failure below
                a reaction handler be a completion instead of a JS_Call.
     resolve  — UNDEFINED because on_loaded's normal return settles NOTHING. Both consumers register the real
                settlement on the module's EVALUATION promise and then return undefined; a resolve here would
                fire the instant the graph loaded and resolve `import()` with undefined instead of the namespace.
                27.5.2.1 drops the completion when the half is undefined, which is exactly the old behaviour of
                the throwaway capability js_promise_then_native used to make — minus the promise nobody held. */
/* `on_loaded` IS A STEP MACHINE, NEVER A C BODY, and that is a property of what it has to do rather than of
   how it happens to be written: both continuations LINK the graph, linking enters each module's own body to
   perform 16.2.1.7.3.1's declaration instantiation, and a flow can be asked for the thread inside one. A C
   reaction body has nowhere to hold the walk across that suspension; a machine's state is exactly where it
   goes. js_new_step_closure builds the bodyless closure — captured data plus the machine — that a reaction
   with work only a machine may do has to be. */
static __exception int js_load_module_then(JSContext *ctx, JSModuleDef *m,
                                           JSValueConst *resolving_funcs,
                                           const JSTrampStepDef *on_loaded)
{
    JSValue loading, mv, reactions[2];
    JSValueConst func_data[3], cap[2];

    loading = js_module_load_requested(ctx, m);
    if (JS_IsException(loading))
        return -1;
    mv = JS_NewModuleValue(ctx, m);
    func_data[0] = resolving_funcs[0];
    func_data[1] = resolving_funcs[1];
    func_data[2] = mv;
    reactions[0] = js_new_step_closure(ctx, on_loaded, 0, 0, 3, func_data);
    reactions[1] = JS_UNDEFINED;
    CHECK(!JS_IsException(reactions[0]),
          "a module graph load could not build its continuation — the graph would load and reach nobody");
    JS_FreeValue(ctx, mv);
    cap[0] = JS_UNDEFINED;
    cap[1] = resolving_funcs[1];
    CHECK(perform_promise_then(ctx, loading, vc(reactions), cap) == 0,
          "a module graph load's continuation could not be attached — the import would never settle");
    JS_FreeValue(ctx, reactions[0]);
    JS_FreeValue(ctx, loading);
    return 0;
}

/* THE EMBEDDER'S "run a module script", once the graph is loaded: 16.2.1.6.1.2 Link ( ), then
   16.2.1.6.1.3 Evaluate ( ), and this load's own capability is settled with the promise Evaluate returns — a
   promise for undefined, not for the namespace, which is the whole difference between this tail and
   13.3.10.3's. The closure's data is [resolve, reject, module].
   ITS CITATION USED TO BE HTML §8.1.3.3 "run a module script" AND WAS WRONG TWICE. That algorithm is in
   §8.1.4.4 Calling scripts (§8.1.3.3 is a fetch clause), and it does not link at all — it Evaluates a record
   the graph fetch has already linked. This entry is quickjs's own embedder path, so what it performs is the
   two ECMAScript concrete methods and it says so.
   A LINK OR CAPABILITY FAILURE IS THIS MACHINE'S OWN THROW, delivered to the load's reject as phase 1 of the
   flow it runs on — see the note where js_load_module_reject_pending used to stand. */
#define MODRUN_STAGES(X) \
    X(MODRUN_LINK,     "16.2.1.6.1.2 Link ( ) step 3 (InnerModuleLinking), resting once per module of the " \
                       "graph because each one's 16.2.1.7.3.1 declaration instantiation is a CALL of that " \
                       "module's own body") \
    X(MODRUN_SETTLE,   "the embedder's own load capability is RESOLVED with 16.2.1.6.1.3 Evaluate ( )'s " \
                       "promise. A CALL, because that resolving function is 27.5.1.3 CreateResolvingFunctions " \
                       "( toResolve )'s for the intrinsic capability and the page's own for a subclassed one") \
    X(MODRUN_EVALUATE, "16.2.1.6.1.3 Evaluate ( )")
enum { MODRUN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_module_loaded_run_steps[] = { MODRUN_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_module_loaded_run_step(JSContext *ctx, void *st, JSValue cb_result,
                                     JSValue **out_cb, int *out_argc)
{
    JSModuleLoaded *s = st;
    JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
    JSModuleDef *m;
    int r;

    DCHECK(JS_VALUE_GET_TAG(rec->data[2]) == JS_TAG_MODULE,
           "a load continuation was handed something that is not the module record it loaded");
    m = JS_VALUE_GET_PTR(rec->data[2]);

    STEP_DISPATCH(MODRUN_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(MODRUN_LINK);
        if (!s->op.started) {
            s->cap = JS_UNDEFINED;
            s->settle_res = JS_UNDEFINED;
            s->settle_cb[0] = s->settle_cb[1] = s->settle_cb[2] = JS_UNDEFINED;
            s->settle_phase = 0;
            s->ev.started = 0;
            s->ev.frames = NULL;
            s->ev.cb[0] = s->ev.cb[1] = JS_UNDEFINED;
            s->ev.res = JS_UNDEFINED;
        }
        r = step_module_link_run(ctx, &s->op, m, cb_result, out_cb, out_argc);
        if (r)
            return r < 0 ? JS_STEP_ABRUPT : r;
        /* THE CAPABILITY BEFORE EVALUATING, and this promise adopts it BEFORE evaluating too — the body can
           FORK, and a sibling forked mid-body has to find this continuation already registered in the world
           it inherits. */
        s->cap = js_module_eval_capability(ctx, m);
        if (JS_IsException(s->cap)) {
            s->cap = JS_UNDEFINED;
            return JS_STEP_ABRUPT;
        }
        STEP_GOTO(s->hdr.stage, MODRUN_SETTLE, &s->op.phase, NULL);
        /* NOT A REST POINT: 27.5.1.5 NewPromiseCapability ( ctor ) over the INTRINSIC Promise is one O(1)
           engine action and runs nothing (js_module_eval_capability passes no constructor). The SETTLE below is
           a request because `resolve` is the CALLER's, not because the capability is. */
        cb_result = JS_UNDEFINED;
        STEP_JUMP(MODRUN_SETTLE);

    STEP_ARM(MODRUN_SETTLE);
        /* `resolve` IS PAGE CODE whenever this load's capability came from a promise SUBCLASS, and the native
           one is a step machine — so it is a CALL request, not a JS_Call from C. As a JS_Call it was an
           activation with no flow base under it, which is the drive-to-completion this engine aborts on the
           moment anything in that resolving function loops, awaits or reads through a getter. */
        {
            JSValueConst arg = s->cap;
            r = step_call_run(ctx, &s->settle_phase, STEP_CB(s->settle_cb), rec->data[0], JS_UNDEFINED,
                              1, &arg, cb_result, &s->settle_res, out_cb, out_argc);
            if (r)
                return r < 0 ? JS_STEP_ABRUPT : r;
        }
        if (JS_IsException(s->settle_res)) {   /* a throwing resolve is this machine's own throw */
            s->settle_res = JS_UNDEFINED;
            return JS_STEP_ABRUPT;
        }
        JS_FreeValue(ctx, s->settle_res);
        s->settle_res = JS_UNDEFINED;
        JS_FreeValue(ctx, s->cap);
        s->cap = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, MODRUN_EVALUATE, &s->settle_phase, &s->op.phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(MODRUN_EVALUATE);
        /* The capability exists, so the only failure Evaluate can still report is the one that creates it;
           every evaluation error settles that capability instead. */
        r = step_module_evaluate_run(ctx, &s->ev, m, cb_result, out_cb, out_argc);
        DCHECK(r >= 0, "module evaluation threw after its capability existed — an evaluation error settles the "
                       "capability, it does not throw past it");
        if (r)
            return r < 0 ? JS_STEP_ABRUPT : r;
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_module_loaded_run_def = {
    sizeof(JSModuleLoaded), js_module_loaded_run_step, NULL, 0,
    .catches_abrupt = 1,   /* a CALL that fails to dispatch is the link's, the settle's or the evaluation's throw */
    .visit = js_module_loaded_visit,
    .algorithm = "the embedder's run-a-module-script tail: 16.2.1.6.1.2 Link ( ) then 16.2.1.6.1.3 Evaluate ( )",
    .steps = js_module_loaded_run_steps
};

static void JS_LoadModuleInternal(JSContext *ctx, const char *basename,
                                  const char *filename,
                                  JSValueConst *resolving_funcs,
                                  JSValueConst attributes)
{
    JSModuleDef *m;
    JSValue pending, name_val, reactions[2];
    JSValueConst func_data[3], cap[2];

    m = js_host_resolve_imported_module(ctx, basename, filename, attributes);
    if (m) {
        /* AN EMBEDDER ENTRY, so a failure has no flow to be the completion of: both callers are plain C
           (JS_LoadModule, and js_dynamic_import_job, which is a job the pump runs directly). */
        if (js_load_module_then(ctx, m, resolving_funcs, &js_module_loaded_import_def) < 0)
            js_load_module_reject_at_entry(ctx, resolving_funcs);
        return;
    }
    pending = js_module_take_pending(ctx, &name_val);
    if (JS_IsUndefined(pending)) {
        js_load_module_reject_at_entry(ctx, resolving_funcs);
        return;
    }
    /* 16.2.1.11: the host finishes the load later. The capability is kept alive by the reactions, so this
       returns with the import still pending and the flow that issued it still suspended on it.
       THE NAME IS THE RESOLVED SPECIFIER THE HANDOFF CARRIED, not `filename`. `filename` is what the page
       WROTE — `./chunk.js` — and registering the record under that would key the module map by a name no
       lookup ever asks for, so every import of one file compiled and evaluated it again. */
    func_data[0] = resolving_funcs[0];
    func_data[1] = resolving_funcs[1];
    func_data[2] = name_val;
    reactions[0] = js_new_step_closure(ctx, &js_module_source_def, 1, 0, 3, func_data);
    reactions[1] = JS_NewCFunctionData(ctx, js_module_source_rejected, 1, 0, 3, func_data);
    /* CHECKED, because perform_promise_then does not report a handler that is an EXCEPTION — JS_IsFunction says
       no and it is filed as "no handler", so an OOM here would silently turn the arriving source into a
       discarded completion. js_promise_then_native hid the same failure one layer further out, in the
       JS_FreeValue its result was passed straight to. */
    CHECK(!JS_IsException(reactions[0]) && !JS_IsException(reactions[1]),
          "a module source load could not build its continuations — the source would arrive and reach nobody");
    JS_FreeValue(ctx, name_val);
    /* THE DERIVED CAPABILITY IS THE IMPORT'S REJECT, with an UNDEFINED resolve — the same pair, and the same two
       reasons, as js_load_module_then's: it carries js_module_source_def's THROW (which is every failure
       of the compile, now that the settle is a completion rather than a JS_Call), and it settles nothing when
       that handler returns normally, because what it returns to is a load it has just registered on. */
    cap[0] = JS_UNDEFINED;
    cap[1] = resolving_funcs[1];
    CHECK(perform_promise_then(ctx, pending, vc(reactions), cap) == 0,
          "a module source load's continuation could not be attached — the import would never settle");
    JS_FreeValue(ctx, reactions[0]);
    JS_FreeValue(ctx, reactions[1]);
    JS_FreeValue(ctx, pending);
}

/* Return a promise or an exception in case of memory error. Used by
   os.Worker() */
JSValue JS_LoadModule(JSContext *ctx, const char *basename,
                      const char *filename)
{
    JSValue promise, resolving_funcs[2];

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;
    JS_LoadModuleInternal(ctx, basename, filename, vc(resolving_funcs), JS_UNDEFINED);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_dynamic_import_job(JSContext *ctx,
                                     int argc, JSValueConst *argv)
{
    JSValueConst *resolving_funcs = argv;
    JSValueConst basename_val = argv[2];
    JSValueConst specifier = argv[3];
    JSValueConst attributes = argv[4];
    const char *basename = NULL, *filename;
    JSValue ret, err;

    if (!JS_IsString(basename_val)) {
        JS_ThrowTypeError(ctx, "no function filename for import()");
        goto exception;
    }
    basename = JS_ToCString(ctx, basename_val);
    if (!basename)
        goto exception;

    filename = JS_ToCString(ctx, specifier);
    if (!filename)
        goto exception;

    JS_LoadModuleInternal(ctx, basename, filename, resolving_funcs, attributes);
    JS_FreeCString(ctx, filename);
    JS_FreeCString(ctx, basename);
    return JS_UNDEFINED;
 exception:
    err = JS_GetException(ctx);
    ret = js_promise_settle_native(ctx, resolving_funcs[1], vc(&err));
    JS_FreeValue(ctx, ret); /* XXX: what to do if exception ? */
    JS_FreeValue(ctx, err);
    JS_FreeCString(ctx, basename);
    return JS_UNDEFINED;
}

/* ---- import()'s options, 13.3.10.2 EvaluateImportCall steps 8-11, as a step machine ----

   js_dynamic_import ran three of the page's operations from C: `Get(options, "with")`, the attribute record's
   `ownKeys` + per-key `getOwnPropertyDescriptor`, and the `Get` of each attribute's VALUE. Every one of them is
   a trap or an accessor, and a loop in any of them had no flow base — the specifier's ToString was already a
   request (CONT_IMPORT), so this was the rest of the same opcode still in C.

   And the walk was in the WRONG ORDER. 7.3.23 with kind key+value reads each surviving key's value between
   that key's descriptor and the NEXT key's descriptor; the C body collected every descriptor first and then
   every value, which a Proxy sees as a different sequence. The cursor performs the whole operation now, so the
   order is the algorithm's rather than the implementation's.

   Every failure here REJECTS the promise instead of throwing — 13.3.10.2 wraps steps 8 onward in
   IfAbruptRejectPromise — so the machine's result is the promise whatever happens, and a throwing trap comes
   back to it as a value (catches_abrupt). That also unifies the two capabilities the old code created: the
   coercion's, made by OP_import for exactly this rejection, and js_dynamic_import's own. */
typedef struct JSImportOpts {
    JSStepHdr hdr;         /* MUST be first — argv[0] = specifier (a primitive), argv[1] = options */
    JSValue promise;       /* the capability's promise: this machine's result on every path (owned) */
    JSValue funcs[2];      /* [resolve, reject] (owned) */
    JSValue basename_val;  /* the referrer, read before anything can suspend (owned) */
    JSValue spec_str;      /* ToString(specifier) (owned) */
    JSValue attrs_obj;     /* options.with (owned) */
    JSValue attrs;         /* the null-prototype attribute record being built (owned) */
    JSEnumKeys *ek;        /* 7.3.23 over attrs_obj, key+value (owned) */
    uint8_t call_phase;    /* step_call_run's cursor for the resolve below */
    JSValue call_cb[3];    /* [this, resolve, namespace]: the call request's buffer (owned while it stands) */
} JSImportOpts;

/* The abrupt completion of any of steps 8-11 is a REJECTION. `err` is consumed. Always returns 0: the opcode
   completes normally with the promise, which is the whole point of IfAbruptRejectPromise. */
static int js_import_opts_reject(JSContext *ctx, JSImportOpts *s, JSValue err)
{
    JSValue rr = js_promise_settle_native(ctx, s->funcs[1], vc(&err));
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, rr);
    return 0;
}

/* step 12: hand the resolved specifier and attribute record to the host. The load cannot run synchronously —
   it would recurse into Evaluate — so it is a job, and nothing here runs the page's code. */
static int js_import_opts_enqueue(JSContext *ctx, JSImportOpts *s)
{
    JSValue args[5];
    args[0] = s->funcs[0]; args[1] = s->funcs[1];
    args[2] = s->basename_val; args[3] = s->spec_str; args[4] = s->attrs;
    JS_EnqueueJob(ctx, js_dynamic_import_job, 5, vc(args));
    return 0;
}

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). This machine's
   prose numbered the operation 16.2.1.8, which is Synthetic Module Records; dynamic import is 13.3.10.2
   EvaluateImportCall, and its step letters here were an edition behind as well. */
#define IMPOPTS_STAGES(X) \
    X(IMPOPTS_SPEC,  "13.3.10.2 steps 1-2 and 7-11.b (referrer; promiseCapability; specifierString is " \
                     "ToString(specifier), already primitive; attributes is a new empty List; a non-Object " \
                     "options rejects) - and the dispatch of step 11.b's Get(options, \"with\")") \
    X(IMPOPTS_WITH,  "13.3.10.2 steps 11.b-11.d.i (attributesObj arrives; undefined leaves the attribute list " \
                     "empty and a non-Object rejects)") \
    X(IMPOPTS_ATTRS, "13.3.10.2 steps 11.d.ii-11.d.iv (entries is EnumerableOwnProperties(attributesObj, " \
                     "key+value) - the ownKeys trap, then per key a getOwnPropertyDescriptor and a Get, " \
                     "interleaved in that order - and each entry's key and value are validated)") \
    X(IMPOPTS_RESOLVE, "13.3.10.3 ContinueDynamicImport step 6.d.ii over an unknown specifier's namespace " \
                       "(Call(promiseCapability.[[Resolve]], undefined, << namespace >>) has returned)")
enum { IMPOPTS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_import_opts_steps[] = { IMPOPTS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_import_opts_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSImportOpts *s = st;
    JSValueConst options = step_arg(&s->hdr, 1);
    int r;

    if (s->hdr.stage == IMPOPTS_SPEC) {
        JSAtom basename;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = JS_UNDEFINED; s->funcs[0] = JS_UNDEFINED; s->funcs[1] = JS_UNDEFINED;
        s->basename_val = JS_UNDEFINED; s->spec_str = JS_UNDEFINED;
        s->attrs_obj = JS_UNDEFINED; s->attrs = JS_UNDEFINED;
        s->call_cb[0] = JS_UNDEFINED; s->call_cb[1] = JS_UNDEFINED; s->call_cb[2] = JS_UNDEFINED;
        /* the REFERRER, read here because it comes off the live stack frame and the machine is about to
           suspend into somewhere else entirely. */
        basename = JS_GetScriptOrModuleName(ctx, 0);
        s->basename_val = (basename == JS_ATOM_NULL) ? JS_NULL : JS_AtomToValue(ctx, basename);
        JS_FreeAtom(ctx, basename);
        if (JS_IsException(s->basename_val)) { s->basename_val = JS_UNDEFINED; return -1; }
        /* the capability, before anything that can fail: from here every failure is a rejection of it, so it
           has to exist first. Creating one runs nothing — it is the intrinsic Promise. */
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        if (JS_IsException(s->promise)) { s->promise = JS_UNDEFINED; return -1; }
        s->hdr.stage = IMPOPTS_WITH;

        /* step 8: the specifier is a PRIMITIVE by now — an object one was coerced by OP_import's ToPrimitive
           request before the opcode resumed — so this runs nothing. It still throws for a Symbol, and that
           throw rejects like every other.
           UNKNOWN EXTERNAL INPUT REACHES HERE UNCOERCED, because js_toprim_operand excludes a concolic (§7.1.1
           over one is the identity) — so the operator answers before the coercion, which owes C a real
           JSString and can only abort. */
        /* Over an unknown specifier there is nothing to fetch, link or evaluate, so the operator answers: the
           namespace the capability settles with is itself unknown, and settling is a value, not a rejection, so
           `import(x).then(m => m.render())` keeps running with the source attached. §13.3.10.3 step 6.d.ii's
           Call of the resolve function performs §27.5.1.3 step 2.f's Get(namespace, "then"), so it is a call
           request and the resolving function's own machine runs it on the trampoline. */
        {
            JSValue ns = js_concolic_derive(ctx, step_arg(&s->hdr, 0), "import", JS_UNDEFINED), out;

            if (!JS_IsUninitialized(ns)) {
                s->hdr.stage = IMPOPTS_RESOLVE;
                r = step_call_run(ctx, &s->call_phase, STEP_CB(s->call_cb), s->funcs[0],
                                  JS_UNDEFINED, 1, vc(&ns), JS_UNDEFINED, &out, out_cb, out_argc);
                JS_FreeValue(ctx, ns);
                DCHECK(r == JS_STEP_CALL, "a call request answered before it was issued");
                return r;
            }
        }
        s->spec_str = JS_ToString(ctx, step_arg(&s->hdr, 0));
        if (JS_IsException(s->spec_str)) {
            s->spec_str = JS_UNDEFINED;
            return js_import_opts_reject(ctx, s, JS_GetException(ctx));
        }
        if (JS_IsUndefined(options))
            return js_import_opts_enqueue(ctx, s);          /* steps 10-11: no options, no attributes */
        if (!JS_IsObject(options)) {
            JS_ThrowTypeError(ctx, "options must be an object");   /* step 11.a */
            return js_import_opts_reject(ctx, s, JS_GetException(ctx));
        }
        /* step 11.b: `Get(options, "with")` — an accessor or a Proxy `get` trap. */
        return step_getprop_run(ctx, &s->hdr, options, JS_ATOM_with, JS_UNDEFINED,
                                &s->attrs_obj, out_cb, out_argc);
    }

    if (s->hdr.stage == IMPOPTS_WITH) {
        /* THE REQUEST IS ENDED FIRST, WHATEVER ITS COMPLETION WAS. This used to test cb_result and return
           BEFORE the read's answer ran, which was sound only while the driver rewound the cursor on an abrupt
           delivery — and that rewind is the defect that hung two other components. The read ends here on both
           completions and REPORTS an abrupt one as -1; step 11.b is inside the IfAbruptRejectPromise, so that
           is what this rejects with. */
        if (step_getprop_done(ctx, &s->hdr, JS_ATOM_NULL, cb_result, &s->attrs_obj) < 0)
            return js_import_opts_reject(ctx, s, JS_GetException(ctx));
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(s->attrs_obj))
            return js_import_opts_enqueue(ctx, s);          /* step 11.d: no `with`, no attributes */
        if (!JS_IsObject(s->attrs_obj)) {
            JS_ThrowTypeError(ctx, "options.with must be an object");   /* step 11.d.i */
            return js_import_opts_reject(ctx, s, JS_GetException(ctx));
        }
        s->attrs = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(s->attrs)) { s->attrs = JS_UNDEFINED; return -1; }
        /* step 11.d.ii: `EnumerableOwnProperties(attributesObj, key+value)` — the `ownKeys` trap, then per key
           a `getOwnPropertyDescriptor` and, if it survives, a `get`, INTERLEAVED in that order. */
        s->ek = js_mallocz(ctx, sizeof(*s->ek));
        if (!s->ek) return -1;
        js_enum_keys_init(s->ek);
        s->ek->obj = js_dup(s->attrs_obj);
        s->ek->want_values = 1;
        s->hdr.stage = IMPOPTS_ATTRS;
        cb_result = JS_UNDEFINED;
    }

    if (s->hdr.stage == IMPOPTS_RESOLVE) {
        JSValue out;

        r = step_call_run(ctx, &s->call_phase, STEP_CB(s->call_cb), JS_UNDEFINED, JS_UNDEFINED, 1,
                          NULL, cb_result, &out, out_cb, out_argc);
        DCHECK(r == 0, "the resolve call request parked twice");
        if (JS_IsException(out)) return -1;   /* step 6.d.ii's `!`: only an internal failure throws */
        JS_FreeValue(ctx, out);
        return 0;
    }
    DCHECK(s->hdr.stage == IMPOPTS_ATTRS, "import()'s options machine resumed in no stage");
    if (JS_IsException(cb_result))
        return js_import_opts_reject(ctx, s, JS_GetException(ctx));
    r = js_enum_keys_run(ctx, &s->hdr, s->ek, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return js_import_opts_reject(ctx, s, JS_GetException(ctx));
    js_enum_keys_keep_enumerable(ctx, s->ek);
    {
        uint32_t k;
        for (k = 0; k < s->ek->kept; k++) {
            if (!JS_IsString(s->ek->vals[k])) {
                JS_ThrowTypeError(ctx, "module attribute values must be strings");
                return js_import_opts_reject(ctx, s, JS_GetException(ctx));
            }
            if (JS_DefinePropertyValue(ctx, s->attrs, s->ek->atoms[k].atom,
                                       js_dup(s->ek->vals[k]), JS_PROP_C_W_E) < 0)
                return js_import_opts_reject(ctx, s, JS_GetException(ctx));
        }
    }
    if (ctx->rt->module_check_attrs
        && ctx->rt->module_check_attrs(ctx, ctx->rt->module_loader_opaque, s->attrs) < 0)
        return js_import_opts_reject(ctx, s, JS_GetException(ctx));
    return js_import_opts_enqueue(ctx, s);
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The capability it answers with, the referrer and specifier it
   read before anything could suspend, the options object and the record it is building from it, and the key
   cursor walking that object — one cursor, declared as the single-element block it is. */
static void js_import_opts_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSImportOpts *s = st;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    v->val(ctx, &s->basename_val);
    v->val(ctx, &s->spec_str);
    v->val(ctx, &s->attrs_obj);
    v->val(ctx, &s->attrs);
    v->array(ctx, (void **)&s->ek, sizeof(JSEnumKeys), 1, 1, js_enum_keys_visit);
    v->val(ctx, &s->call_cb[0]);
    v->val(ctx, &s->call_cb[1]);
    v->val(ctx, &s->call_cb[2]);
}

static JSValue js_import_opts_fini(JSContext *ctx, void *st, bool take_result)
{
    JSImportOpts *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    if (take_result) s->promise = JS_UNDEFINED;
    return r;
}

/* DELETED: js_dynamic_import. Its options walk is the machine above; its capability is the machine's, which is
   also the one OP_import used to create separately for the specifier coercion's rejection — two capabilities
   for one promise, of which the first was thrown away on the ordinary path. */

