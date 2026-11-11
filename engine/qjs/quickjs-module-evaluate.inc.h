static void js_set_module_evaluated(JSContext *ctx, JSModuleDef *m)
{
    js_module_set_status(ctx, m, JS_MODULE_STATUS_EVALUATED);
    if (!JS_IsUndefined(m->promise)) {
        JSValue ret_val;
        DCHECK(m->cycle_root == m, "m->cycle_root == m");
        JSValueConst value = JS_UNDEFINED;
        ret_val = js_promise_settle_native(ctx, m->resolving_funcs[0], &value);
        JS_FreeValue(ctx, ret_val);
    }
}

typedef struct {
    JSModuleDef **tab;
    int count;
    int size;
} ExecModuleList;

/* XXX: slow. Could use a linked list instead of ExecModuleList */
static bool find_in_exec_module_list(ExecModuleList *exec_list, JSModuleDef *m)
{
    int i;
    for(i = 0; i < exec_list->count; i++) {
        if (exec_list->tab[i] == m)
            return true;
    }
    return false;
}

/* AsyncModuleExecutionFulfilled's GatherAvailableAncestors, as an explicit walk rather than C recursion, and
   therefore with no stack guard: the depth was the async module GRAPH's, which the imported source picks, and
   the js_check_stack_overflow in front of it turned a graph the algorithm answers into a RangeError.
   THE WORKLIST IS exec_list ITSELF. Every module this appends is one whose dependencies just reached zero,
   which is exactly the set the recursion descended into — so a cursor over the entries appended by this call
   visits each exactly once, and no second array is needed. The one asymmetry the cursor must respect is that a
   module WITH top-level await is appended but NOT expanded, the same test the recursion made before calling
   itself.
   The traversal order changes from depth-first to breadth-first and that is not observable: the SET reached is
   identical (an edge decrements pending_async_dependencies once either way, and a module is appended only on
   the decrement that reaches zero), and the caller sorts exec_list by async_evaluation_timestamp before using
   it. */
/* WILL THIS MODULE GET ITS OWN FULFILLED REACTION? Exactly one predicate, because two sites must agree about
   it and they did not. Stock quickjs evaluates a non-TLA module synchronously, so GatherAvailableAncestors has
   to expand it INLINE — nothing else will ever propagate to its parents. This fork evaluates every BYTECODE
   body asynchronously whether or not it has top-level await, because a loop or await back-edge can park it,
   and only a C init_func module stays synchronous. The executor was updated for that; the gather was not, so a
   non-TLA bytecode module was expanded inline AND later ran its own fulfilled reaction, and each of its
   parents had pending_async_dependencies decremented twice. The second decrement is the one that goes below
   zero.
   That was a plain assert, compiled out of the NDEBUG build the harness uses, so the corpus was green over it
   for as long as it existed. Migrating the asserts to DCHECK is what made it visible. */
static inline bool js_module_is_async_evaluated(const JSModuleDef *m)
{
    return m->has_tla || !m->init_func;
}

static int gather_available_ancestors(JSContext *ctx, JSModuleDef *module,
                                      ExecModuleList *exec_list)
{
    int i, cursor = exec_list->count;
    JSModuleDef *cur = module;

    for (;;) {
        for(i = 0; i < cur->async_parent_modules_count; i++) {
            JSModuleDef *m = cur->async_parent_modules[i];
            DCHECK(m->cycle_root != NULL,
                   "§16.2.1.6.1.3.3 GatherAvailableAncestors ( module, execList ) read an async parent whose "
                   "[[CycleRoot]] is unset — a parent is appended during its own InnerModuleEvaluation, whose SCC "
                   "pop sets the field before any job can run, so in this flow's world that walk never reached its "
                   "pop: find where this world's walk (step_module_evaluate_run) stopped");
            if (!find_in_exec_module_list(exec_list, m) &&
                !m->cycle_root->eval_has_exception) {
                /* THE DECREMENT BELOW IS A WRITE TO m, not to the module whose promise settled, and it was the
                   only write to a captured field in the whole module system that never announced itself. The
                   caller captures the fulfilled module; every ANCESTOR whose pending count this walk lowers is
                   a different module, so without this the decrement lands outside the flow's COW delta and is
                   seen by every sibling flow. Two flows then decrement the same counter once each, it reaches
                   zero twice, and the second walk trips "pending_async_dependencies > 0" — which was a plain
                   assert compiled out by -DNDEBUG, so it went unseen. */
                js_module_eval_capture(ctx, m);
                DCHECK(m->status == JS_MODULE_STATUS_EVALUATING_ASYNC, "m->status == JS_MODULE_STATUS_EVALUATING_ASYNC");
                DCHECK(!m->eval_has_exception, "!m->eval_has_exception");
                DCHECK(m->async_evaluation, "m->async_evaluation");
                DCHECK(m->pending_async_dependencies > 0, "m->pending_async_dependencies > 0");
                m->pending_async_dependencies--;
                if (m->pending_async_dependencies == 0) {
                    if (js_resize_array(ctx, (void **)&exec_list->tab, sizeof(exec_list->tab[0]), &exec_list->size, exec_list->count + 1)) {
                        return -1;
                    }
                    exec_list->tab[exec_list->count++] = m;
                }
            }
        }
        /* The next appended entry that still needs expanding. A module that will run its own fulfilled
           reaction is a LEAF here — that reaction re-enters this walk for it, and expanding it now as well is
           the double-decrement. */
        while (cursor < exec_list->count &&
               js_module_is_async_evaluated(exec_list->tab[cursor]))
            cursor++;
        if (cursor >= exec_list->count)
            return 0;
        cur = exec_list->tab[cursor++];
    }
}

static int exec_module_list_cmp(const void *p1, const void *p2, void *opaque)
{
    JSModuleDef *m1 = *(JSModuleDef **)p1;
    JSModuleDef *m2 = *(JSModuleDef **)p2;
    return (m1->async_evaluation_timestamp > m2->async_evaluation_timestamp) -
        (m1->async_evaluation_timestamp < m2->async_evaluation_timestamp);
}

static int js_execute_sync_module(JSContext *ctx, JSModuleDef *m,
                                  JSValue *pvalue);

/* AsyncModuleExecutionRejected, as an explicit STACK rather than C recursion, and so with no stack guard: the
   depth was the async module graph's, which the imported source picks.

   THE ORDER IS THE WHOLE DIFFICULTY, and it is why this is a LIFO stack and not the cursor-over-exec_list that
   GatherAvailableAncestors uses. Step 9 rejects THIS module's capability and step 10 then walks the async
   parents in index order, so settlement is pre-order depth-first and OBSERVABLE — a test measures it. A
   worklist consumed front-to-back would turn that into breadth-first and reorder promise settlement. Popping
   from a stack and pushing the parents in REVERSE reproduces the recursion's order exactly: parent 0 is on top
   and is fully expanded before parent 1 is reached. Order preserved by CONSTRUCTION, not by argument.

   The recursion also allocated a JSValue per edge — JS_NewModuleValue purely so the recursive call could
   re-enter through this function's own callback signature. Walking directly needs no such wrapper, so the
   edge allocation goes with the recursion. */
static JSValue js_async_module_execution_rejected(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv, int magic,
                                                  JSValueConst *func_data)
{
    JSModuleDef *module = JS_VALUE_GET_PTR(func_data[0]);
    JSValueConst error = argv[0];
    JSModuleDef **stack = NULL;
    int stack_count = 0, stack_size = 0;
    int i;

    if (js_resize_array(ctx, (void **)&stack, sizeof(stack[0]), &stack_size, 1))
        return JS_EXCEPTION;
    stack[stack_count++] = module;

    while (stack_count > 0) {
        JSModuleDef *m = stack[--stack_count];

        js_module_eval_capture(ctx, m);   /* this reaction is a WRITE to the module's evaluation state */
        if (m->status == JS_MODULE_STATUS_EVALUATED) {
            DCHECK(m->eval_has_exception, "m->eval_has_exception");
            continue;
        }

        DCHECK(m->status == JS_MODULE_STATUS_EVALUATING_ASYNC, "m->status == JS_MODULE_STATUS_EVALUATING_ASYNC");
        DCHECK(!m->eval_has_exception, "!m->eval_has_exception");
        DCHECK(m->async_evaluation, "m->async_evaluation");

        m->eval_has_exception = true;
        m->eval_exception = js_dup(error);
        js_module_set_status(ctx, m, JS_MODULE_STATUS_EVALUATED);

        /* step 9, before any parent is reached — the leaf-to-root settlement order */
        if (!JS_IsUndefined(m->promise)) {
            JSValue ret_val;
            DCHECK(m->cycle_root == m, "m->cycle_root == m");
            ret_val = js_promise_settle_native(ctx, m->resolving_funcs[1], &error);
            JS_FreeValue(ctx, ret_val);
        }

        /* step 10, pushed in REVERSE so they pop in index order */
        if (js_resize_array(ctx, (void **)&stack, sizeof(stack[0]), &stack_size,
                            stack_count + m->async_parent_modules_count)) {
            js_free(ctx, stack);
            return JS_EXCEPTION;
        }
        for(i = m->async_parent_modules_count - 1; i >= 0; i--)
            stack[stack_count++] = m->async_parent_modules[i];
    }
    js_free(ctx, stack);
    return JS_UNDEFINED;
}

static void js_module_eval_register(JSContext *ctx, JSModuleDef *m, JSValueConst promise);

/* 16.2.1.6.1.3.4 AsyncModuleExecutionFulfilled ( module ) as a step machine. Step 12 runs each available
   ancestor's body inside its loop, so each body is a CALL request on the running flow's chain and the loop's
   cursor lives here: a park or a fork inside a body carries the rest of the loop. The closure's data is
   [module]; the completion is undefined. */
typedef struct JSModuleFulfilled {
    JSStepHdr hdr;              /* must be first */
    ExecModuleList list;        /* step 11's sortedExecList; `tab` is OWNED and declared by the visit */
    int i;                      /* step 12's cursor into it */
    JSModuleDef *body;          /* the ancestor whose body CALL is in flight, NULL when none is */
    JSValue cb[2];              /* that CALL's operands: [this = undefined, the ancestor's body] */
    JSValue res;                /* its completion: the promise the async body returns */
    uint8_t phase;              /* step_call_run's own cursor */
    uint8_t started;
} JSModuleFulfilled;
_Static_assert(offsetof(JSModuleFulfilled, hdr) == 0, "JSStepHdr must be first in JSModuleFulfilled");

#define MODFUL_STAGES(X) \
    X(MODFUL_ANCESTORS, "16.2.1.6.1.3.4 AsyncModuleExecutionFulfilled ( module ) step 12, resting at each " \
                        "ancestor, whose 16.2.1.6.1.3.2 ExecuteAsyncModule ( module ) step 9 ExecuteModule is a " \
                        "CALL of that module's own body")
enum { MODFUL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_module_fulfilled_steps[] = { MODFUL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_module_fulfilled_step(JSContext *ctx, void *st, JSValue cb_result,
                                    JSValue **out_cb, int *out_argc)
{
    JSModuleFulfilled *s = st;
    JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
    JSModuleDef *module, *m;
    int r;

    DCHECK(JS_VALUE_GET_TAG(rec->data[0]) == JS_TAG_MODULE,
           "AsyncModuleExecutionFulfilled's closure was handed something that is not the module record it "
           "captures");
    module = JS_VALUE_GET_PTR(rec->data[0]);

    STEP_DISPATCH(MODFUL_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(MODFUL_ANCESTORS);
        if (!s->started) {
            /* every owned field before the first operation that can fail: the teardown visits them all */
            s->list.tab = NULL;
            s->list.count = s->list.size = 0;
            s->i = 0;
            s->body = NULL;
            s->cb[0] = s->cb[1] = JS_UNDEFINED;
            s->res = JS_UNDEFINED;
            s->phase = 0;
            s->started = 1;
            JS_FreeValue(ctx, cb_result);
            /* steps 1-11. This reaction writes the module's evaluation state, so the capture precedes the read. */
            js_module_eval_capture(ctx, module);
            if (module->status == JS_MODULE_STATUS_EVALUATED) {
                DCHECK(module->eval_has_exception, "module->eval_has_exception");
                return JS_STEP_DONE;
            }
            DCHECK(module->status == JS_MODULE_STATUS_EVALUATING_ASYNC, "module->status == JS_MODULE_STATUS_EVALUATING_ASYNC");
            DCHECK(!module->eval_has_exception, "!module->eval_has_exception");
            DCHECK(module->async_evaluation, "module->async_evaluation");
            module->async_evaluation = false;
            js_set_module_evaluated(ctx, module);
            if (gather_available_ancestors(ctx, module, &s->list) < 0)
                return JS_STEP_ABRUPT;
            rqsort(s->list.tab, s->list.count, sizeof(s->list.tab[0]), exec_module_list_cmp, NULL);
        } else if (s->phase != 0) {
            /* the body CALL has completed: ExecuteAsyncModule's PerformPromiseThen on its promise. A throw does
               not come back here; an async body's throw rejects its promise. */
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                              cb_result, &s->res, out_cb, out_argc);
            DCHECK(r == 0,
                   "an ancestor body's CALL request resumed and did not end — step_call_run's second phase "
                   "always completes, so a step code here is a cursor that names a request nobody issued");
            DCHECK(s->body != NULL && s->i < s->list.count && s->list.tab[s->i] == s->body,
                   "an ancestor body's CALL completed for a module that is not step 12's current element");
            js_module_eval_register(ctx, s->body, s->res);
            JS_FreeValue(ctx, s->res);
            s->res = JS_UNDEFINED;
            s->body = NULL;
            s->i++;
            return JS_STEP_YIELD;
        } else {
            JS_FreeValue(ctx, cb_result);   /* re-entered from a JS_STEP_YIELD: the driver delivers undefined */
        }

        if (s->i == s->list.count)
            return JS_STEP_DONE;
        m = s->list.tab[s->i];
        js_module_eval_capture(ctx, m);
        if (m->status == JS_MODULE_STATUS_EVALUATED) {
            /* step 12.a */
            DCHECK(m->eval_has_exception, "m->eval_has_exception");
        } else if (js_module_is_async_evaluated(m)) {
            /* step 12.b. Every bytecode body is async-evaluated, top-level await or not, as
               step_module_evaluate_run decides; only a C init_func module takes step 12.c. */
            DCHECK(JS_VALUE_GET_TAG(m->func_obj) == JS_TAG_OBJECT,
                   "an ancestor reached ExecuteAsyncModule with no body object — js_create_module_function "
                   "builds one for every module of the graph before the link");
            s->body = m;
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), m->func_obj, JS_UNDEFINED, 0, NULL,
                              JS_UNDEFINED, &s->res, out_cb, out_argc);
            DCHECK(r == JS_STEP_CALL,
                   "an ancestor body's CALL request answered something other than a call — its cursor was "
                   "already in flight, so the answer to the old request is about to be collected as this one's");
            return r;
        } else {
            /* step 12.c: a C init_func module, which runs no page code and cannot park */
            JSValue error;
            if (js_execute_sync_module(ctx, m, &error) < 0) {
                JSValue m_obj = JS_NewModuleValue(ctx, m);
                js_async_module_execution_rejected(ctx, JS_UNDEFINED, 1, vc(&error), 0, vc(&m_obj));
                JS_FreeValue(ctx, m_obj);
                JS_FreeValue(ctx, error);
            } else {
                js_set_module_evaluated(ctx, m);
            }
        }
        s->i++;
        /* a rest with nothing to run, offered: sortedExecList is the page's size */
        return JS_STEP_YIELD;
}

static void js_module_fulfilled_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSModuleFulfilled *s = st;
    int k;
    for (k = 0; k < 2; k++)
        v->val(ctx, &s->cb[k]);
    v->val(ctx, &s->res);
    v->buf(ctx, (void **)&s->list.tab, (size_t)s->list.size * sizeof(*s->list.tab));
}

static const JSTrampStepDef js_module_fulfilled_def = {
    sizeof(JSModuleFulfilled), js_module_fulfilled_step, NULL, 0,
    .visit = js_module_fulfilled_visit,
    .algorithm = "16.2.1.6.1.3.4 AsyncModuleExecutionFulfilled ( module )",
    .steps = js_module_fulfilled_steps
};

/* 16.2.1.6.1.3.2 ExecuteAsyncModule ( module )'s PerformPromiseThen: AsyncModuleExecutionFulfilled and
   AsyncModuleExecutionRejected registered on the body's promise. Both callers make the body a CALL request, so
   the CALL returns into its machine in every arm of a fork inside the body and each arm registers. */
static void js_module_eval_register(JSContext *ctx, JSModuleDef *m, JSValueConst promise)
{
    JSValue m_obj, resolve_funcs[2];

    DCHECK(JS_GetOpaque(promise, JS_CLASS_PROMISE) != NULL,
           "a module body's evaluation answered something that is not a promise — every bytecode module body "
           "is an async function, so its call returns one");
    m_obj = JS_NewModuleValue(ctx, m);
    resolve_funcs[0] = js_new_step_closure(ctx, &js_module_fulfilled_def, 0, 0, 1, vc(&m_obj));
    resolve_funcs[1] = JS_NewCFunctionData(ctx, js_async_module_execution_rejected, 0, 0, 1, vc(&m_obj));
    CHECK(!JS_IsException(resolve_funcs[0]) && !JS_IsException(resolve_funcs[1]),
          "a module body's evaluation could not build its continuations — the body would settle and reach "
          "nobody, leaving its ancestors evaluating-async");
    JS_FreeValue(ctx, js_promise_then_native(ctx, promise, vc(resolve_funcs)));
    JS_FreeValue(ctx, m_obj);
    JS_FreeValue(ctx, resolve_funcs[0]);
    JS_FreeValue(ctx, resolve_funcs[1]);
}

/* 16.2.1.6.1.3.1 step 13.a and 16.2.1.6.1.3.4 step 12.c, ExecuteModule for the one kind this engine evaluates
   synchronously: a C init_func module, which runs no page code and cannot park. Every bytecode body is a CALL
   request of its walk instead. return < 0 in case of exception; *pvalue contains the exception. */
static int js_execute_sync_module(JSContext *ctx, JSModuleDef *m,
                                  JSValue *pvalue)
{
    DCHECK(m->init_func != NULL,
           "a bytecode module body reached the synchronous ExecuteModule — both walks run it as a CALL request, "
           "so this caller decided js_module_is_async_evaluated differently from them");
    if (m->init_func(ctx, m) < 0) {
        *pvalue = JS_GetException(ctx);
        return -1;
    }
    *pvalue = JS_UNDEFINED;
    return 0;
}

/* spec: InnerModuleEvaluation. Return (index, JS_UNDEFINED) or (-1,
   exception) */
/* 16.2.1.6.1.3.1 InnerModuleEvaluation, split so the WALK around it is an explicit frame stack rather than C
   recursion — see step_module_evaluate_run below. This is its POST-ORDER half (steps 12-16). */
static int js_module_eval_finish(JSContext *ctx, JSModuleDef *m, JSModuleDef **pstack_top, bool *pexec,
                                 JSValue *pvalue)
{
    JSModuleDef *m1;

    bool exec_async = false;

    *pexec = false;

    /* Announced again for js_module_eval_after_child's reason: the children's bodies ran since the entry. */
    js_module_eval_capture(ctx, m);
    if (m->pending_async_dependencies > 0) {
        DCHECK(!m->async_evaluation, "!m->async_evaluation");
        m->async_evaluation = true;
        m->async_evaluation_timestamp =
            ctx->rt->module_async_evaluation_next_timestamp++;
    } else if (m->has_tla || !m->init_func) {
        /* EVERY bytecode module body is an async function that can PARK on the flow machinery (a loop/await
           back-edge), so it is async-evaluated even without top-level await — the only truly-synchronous modules are
           C init_func modules (no parking). Treating them all as async from the DFS keeps pending_async_dependencies
           correct when a "sync" body parks (a runtime-discovered async-ness would corrupt the parents' counts). */
        DCHECK(!m->async_evaluation, "!m->async_evaluation");
        m->async_evaluation = true;
        m->async_evaluation_timestamp =
            ctx->rt->module_async_evaluation_next_timestamp++;
        exec_async = true;   /* run BELOW, after the SCC pop — see the note there */
    } else {
        /* C init_func module: truly synchronous, never parks. */
        if (js_execute_sync_module(ctx, m, pvalue) < 0)
            return -1;
    }

    DCHECK(m->dfs_ancestor_index <= m->dfs_index, "m->dfs_ancestor_index <= m->dfs_index");
    if (m->dfs_index == m->dfs_ancestor_index) {
        for(;;) {
            /* pop m1 from stack */
            m1 = *pstack_top;
            *pstack_top = m1->stack_prev;
            /* `m1` AND NOT `m`: the pop walks the whole strongly connected component, so these two writes
               are to modules this frame never entered. They were covered because a module reaches this stack
               only through js_module_eval_enter, which announces first — an argument about the walk, which the
               shared helper now makes unnecessary. */
            if (!m1->async_evaluation) {
                js_module_set_status(ctx, m1, JS_MODULE_STATUS_EVALUATED);
            } else {
                js_module_set_status(ctx, m1, JS_MODULE_STATUS_EVALUATING_ASYNC);
            }
            /* spec bug: cycle_root must be assigned before the test */
            m1->cycle_root = m;
            if (m1 == m)
                break;
        }
    }
    /* THE BODY RUNS AFTER THE SCC POP, and the reason is forking. 16.2.1.6.1.3.1 executes at step 13 and pops at
       steps 14-16, which is sound only because the body's asynchronous CONTINUATION cannot run before the pop —
       and AsyncModuleExecutionFulfilled asserts exactly that (`[[Status]] is EVALUATING-ASYNC`). A forked flow
       breaks the "cannot": the sibling's world is the one that existed AT the branch inside the body, where the
       pop had not happened, and it then settles the body's promise and reaches that assert with status
       EVALUATING. The pop is pure state — no page code runs in it — so performing it first makes every arm of a
       fork see the state the continuation requires. It also makes an unhandled case defined rather than
       asserting: a module body that dynamically imports ITSELF reached Evaluate with status EVALUATING, which
       is not one of the three it accepts. The body is the caller's CALL request (step_module_evaluate_run). */
    *pexec = exec_async;
    *pvalue = JS_UNDEFINED;
    return 0;
}

/* Steps 11.b-11.d: what the parent does once a child has been entered (already evaluating) or finished. It is its
   own function because the flattened walk reaches it from two places — a child that needed no visit, and a child
   whose frame has just been popped — which is the one point the recursion reached it from. */
static int js_module_eval_after_child(JSContext *ctx, JSModuleDef *m, JSModuleDef *m1, JSValue *pvalue)
{
    /* Both records are announced here and not only where the walk entered them: a child's body runs between
       that entry and this call, and a fork inside it freezes the running flow's head, so the earlier
       announcement no longer stands for the writes below. */
    js_module_eval_capture(ctx, m);
    js_module_eval_capture(ctx, m1);
    DCHECK(m1->status == JS_MODULE_STATUS_EVALUATING || m1->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m1->status == JS_MODULE_STATUS_EVALUATED, "m1->status == JS_MODULE_STATUS_EVALUATING || m1->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m1->status == JS_MODULE_STATUS_EVALUATED");
    if (m1->status == JS_MODULE_STATUS_EVALUATING) {
        m->dfs_ancestor_index = min_int(m->dfs_ancestor_index,
                            m1->dfs_ancestor_index);
    } else {
        m1 = m1->cycle_root;
        /* The cycle root is a module this walk may never have entered, and the lines below read its status and
           append to its [[AsyncParentModules]], both per-flow fields: announced first, as every write is. */
        js_module_eval_capture(ctx, m1);
        DCHECK(m1->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m1->status == JS_MODULE_STATUS_EVALUATED, "m1->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m1->status == JS_MODULE_STATUS_EVALUATED");
        if (m1->eval_has_exception) {
        *pvalue = js_dup(m1->eval_exception);
        return -1;
        }
    }
    if (m1->async_evaluation) {
        m->pending_async_dependencies++;
        if (js_resize_array(ctx, (void **)&m1->async_parent_modules, sizeof(m1->async_parent_modules[0]), &m1->async_parent_modules_size, m1->async_parent_modules_count + 1)) {
        *pvalue = JS_GetException(ctx);
        return -1;
        }
        m1->async_parent_modules[m1->async_parent_modules_count++] = m;
    }
    return 0;
}

/* The PRE-ORDER half (steps 4-10). *pdid says whether a frame is owed: a module already evaluating or evaluated
   needs no visit, which is the recursion's early return. */
static int js_module_eval_enter(JSContext *ctx, JSModuleDef *m, int index, JSModuleDef **pstack_top,
                                JSValue *pvalue, int *pdid)
{
    *pdid = 0;
    *pvalue = JS_UNDEFINED;
    js_module_eval_capture(ctx, m);   /* the status this reads is per-flow, so capture precedes the read */
#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
    if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
        char buf1[ATOM_GET_STR_BUF_SIZE];
        printf("InnerModuleEvaluation '%s':\n", JS_AtomGetStr(ctx, buf1, sizeof(buf1), m->module_name));
    }
#endif
    if (m->status == JS_MODULE_STATUS_EVALUATING_ASYNC ||
        m->status == JS_MODULE_STATUS_EVALUATED) {
        if (m->eval_has_exception) {
            *pvalue = js_dup(m->eval_exception);
            return -1;
        }
        return index;
    }
    if (m->status == JS_MODULE_STATUS_EVALUATING)
        return index;
    DCHECK(m->status == JS_MODULE_STATUS_LINKED,
           "§16.2.1.6.1.3.1 InnerModuleEvaluation ( module, stack, index ) step 4 asserts the status is "
           "LINKED here and it is not — the three statuses steps 2 and 3 name all return, so this is a "
           "record whose link phase did not finish in THIS flow's world, which is what capturing the "
           "status at every write exists to make impossible");
    js_module_set_status(ctx, m, JS_MODULE_STATUS_EVALUATING);
    m->dfs_index = index;
    m->dfs_ancestor_index = index;
    m->pending_async_dependencies = 0;
    /* push 'm' on stack */
    m->stack_prev = *pstack_top;
    *pstack_top = m;
    *pdid = 1;
    return index + 1;
}

/* The walk's cursor: one frame per module whose 16.2.1.6.1.3.1 child loop (`For each ModuleRequest Record
   request of module.[[RequestedModules]]`) is in progress. The traversal is Tarjan's, threaded exactly as the
   recursion threaded `index` and the dfs/ancestor indices. Nothing caps the frame array. */
typedef struct JSModuleEvalFrame {
    JSModuleDef *m;
    int i;              /* cursor over m->req_module_entries */
} JSModuleEvalFrame;

static int js_module_eval_push(JSContext *ctx, JSModuleEvalOp *op, JSModuleDef *m)
{
    if (op->depth == op->cap) {
        int ncap = op->cap ? op->cap * 2 : 8;
        JSModuleEvalFrame *nf = js_realloc(ctx, op->frames, sizeof(*nf) * (size_t)ncap);
        if (unlikely(!nf))
            return -1;
        op->frames = nf;
        op->cap = ncap;
    }
    op->frames[op->depth].m = m;
    op->frames[op->depth].i = 0;
    op->depth++;
    return 0;
}

/* 16.2.1.6.1.3 Evaluate ( )'s abrupt arm: every module still on `stack` becomes evaluated with `result` as its
   [[EvaluationError]], and the top-level capability is rejected. Takes `result`. */
static void js_module_eval_abrupt(JSContext *ctx, JSModuleEvalOp *op, JSValue result)
{
    JSModuleDef *m = op->root, *m1;
    JSValue ret_val;

    while (op->stack_top != NULL) {
        m1 = op->stack_top;
        DCHECK(m1->status == JS_MODULE_STATUS_EVALUATING, "m1->status == JS_MODULE_STATUS_EVALUATING");
        /* `m1` and not `m`, as in the SCC pop: js_module_set_status announces it. */
        js_module_set_status(ctx, m1, JS_MODULE_STATUS_EVALUATED);
        m1->eval_has_exception = true;
        m1->eval_exception = js_dup(result);
        m1->cycle_root = m; /* spec bug: should be present */
        op->stack_top = m1->stack_prev;
    }
    JS_FreeValue(ctx, result);
    op->depth = 0;
    DCHECK(m->status == JS_MODULE_STATUS_EVALUATED, "m->status == JS_MODULE_STATUS_EVALUATED");
    DCHECK(m->eval_has_exception, "m->eval_has_exception");
    ret_val = js_promise_settle_native(ctx, m->resolving_funcs[1], vc(&m->eval_exception));
    JS_FreeValue(ctx, ret_val);
}

/* Evaluate's normal arm: a graph that evaluated synchronously resolves its capability now; an asynchronous one
   is resolved by AsyncModuleExecutionFulfilled. */
static void js_module_eval_normal(JSContext *ctx, JSModuleEvalOp *op)
{
    JSModuleDef *m = op->root;

    DCHECK(m->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m->status == JS_MODULE_STATUS_EVALUATED, "m->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m->status == JS_MODULE_STATUS_EVALUATED");
    DCHECK(!m->eval_has_exception, "!m->eval_has_exception");
    if (!m->async_evaluation) {
        JSValueConst value = JS_UNDEFINED;
        JSValue ret_val;

        DCHECK(m->status == JS_MODULE_STATUS_EVALUATED, "m->status == JS_MODULE_STATUS_EVALUATED");
        ret_val = js_promise_settle_native(ctx, m->resolving_funcs[0], &value);
        JS_FreeValue(ctx, ret_val);
    }
    DCHECK(op->stack_top == NULL, "16.2.1.6.1.3 Evaluate ( ): `stack` is not empty after its walk returned");
}

static int step_module_evaluate_run(JSContext *ctx, JSModuleEvalOp *op, JSModuleDef *m,
                                    JSValue in, JSValue **out_cb, int *out_argc)
{
    JSModuleEvalFrame *f;
    JSValue result;
    bool exec;
    int nindex, did, r;

    if (!op->started) {
        JSValue cap;

        /* every owned field before the first operation that can fail: the teardown visits them all */
        op->frames = NULL;
        op->depth = op->cap = 0;
        op->index = 0;
        op->stack_top = NULL;
        op->root = NULL;
        op->body = NULL;
        op->cb[0] = op->cb[1] = JS_UNDEFINED;
        op->res = JS_UNDEFINED;
        op->phase = 0;
        op->started = 1;
        JS_FreeValue(ctx, in);
        /* Evaluate's steps up to its [[TopLevelCapability]] test. The capture precedes the READ: whether this
           flow evaluates at all is decided by state a sibling may have written. */
        js_module_eval_capture(ctx, m);
        DCHECK(m->status == JS_MODULE_STATUS_LINKED || m->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m->status == JS_MODULE_STATUS_EVALUATED, "m->status == JS_MODULE_STATUS_LINKED || m->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m->status == JS_MODULE_STATUS_EVALUATED");
        if (m->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m->status == JS_MODULE_STATUS_EVALUATED)
            m = m->cycle_root;
        /* The test is `eval_started` and not the capability: a consumer takes the capability before the body
           runs so it can register first, and reading that as "already evaluated" would return a promise
           nothing settles. */
        if (m->eval_started) {
            DCHECK(!JS_IsUndefined(m->promise),
                   "a module whose evaluation started has no capability — its consumers have nothing to wait on");
            return 0;
        }
        js_module_eval_capture(ctx, m);   /* `m` may be the cycle root, never announced */
        m->eval_started = true;
        cap = js_module_eval_capability(ctx, m);
        if (JS_IsException(cap))
            return -1;
        JS_FreeValue(ctx, cap);
        op->root = m;
        nindex = js_module_eval_enter(ctx, m, 0, &op->stack_top, &result, &did);
        if (nindex < 0)
            goto abrupt;
        op->index = nindex;
        if (!did) {
            js_module_eval_normal(ctx, op);
            return 0;
        }
        if (js_module_eval_push(ctx, op, m) < 0) {
            result = JS_GetException(ctx);
            goto abrupt;
        }
    } else if (op->phase != 0) {
        /* the body CALL has completed: ExecuteAsyncModule's PerformPromiseThen on its promise. A CALL that
           failed to dispatch arrives as JS_EXCEPTION (both machines running this declare catches_abrupt). The
           body is an async function, whose abrupt start rejects its promise (ECMAScript §15.8.4 Runtime
           Semantics: EvaluateAsyncFunctionBody step 3), so the failure is registered as that rejection. */
        r = step_call_run(ctx, &op->phase, STEP_CB(op->cb), JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                          in, &op->res, out_cb, out_argc);
        DCHECK(r == 0,
               "a module body's CALL request resumed and did not end — step_call_run's second phase always "
               "completes, so a step code here is a cursor that names a request nobody issued");
        if (JS_IsException(op->res)) {
            JSValue funcs[2], err = JS_GetException(ctx);

            op->res = JS_NewPromiseCapability(ctx, funcs);
            CHECK(!JS_IsException(op->res),
                  "a module body whose start failed could not have its rejection built — its ancestors would "
                  "stay evaluating-async");
            JS_FreeValue(ctx, js_promise_settle_native(ctx, funcs[1], vc(&err)));
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, err);
        }
        DCHECK(op->body != NULL && op->depth > 0 && op->frames[op->depth - 1].m == op->body,
               "a module body's CALL completed for a module that is not the walk's current frame");
        js_module_eval_register(ctx, op->body, op->res);
        JS_FreeValue(ctx, op->res);
        op->res = JS_UNDEFINED;
        op->body = NULL;
        goto popped;
    } else {
        JS_FreeValue(ctx, in);   /* re-entered from a JS_STEP_YIELD: the driver delivers undefined */
    }

    DCHECK(op->depth > 0, "16.2.1.6.1.3.1's walk advanced with no frame on its path");
    f = &op->frames[op->depth - 1];
    while (f->i < f->m->req_module_entries_count) {
        JSModuleDef *m1 = f->m->req_module_entries[f->i].module;

        f->i++;
        nindex = js_module_eval_enter(ctx, m1, op->index, &op->stack_top, &result, &did);
        if (nindex < 0)
            goto abrupt;
        op->index = nindex;
        if (did) {
            if (js_module_eval_push(ctx, op, m1) < 0) {
                result = JS_GetException(ctx);
                goto abrupt;
            }
            /* a rest with nothing to run, offered: the graph is the page's size */
            return JS_STEP_YIELD;
        }
        if (js_module_eval_after_child(ctx, f->m, m1, &result) < 0)
            goto abrupt;
    }
    if (js_module_eval_finish(ctx, f->m, &op->stack_top, &exec, &result) < 0)
        goto abrupt;
    if (exec) {
        /* ExecuteModule as a CALL of the module's body with `this` undefined, which selects its code over its
           16.2.1.7.3.1 prologue. Its frame lands on this flow's chain, so a park or a fork inside it holds the
           walk too. */
        DCHECK(JS_VALUE_GET_TAG(f->m->func_obj) == JS_TAG_OBJECT,
               "a module reached ExecuteModule with no body object — js_create_module_function builds one for "
               "every module of the graph before the link");
        op->body = f->m;
        r = step_call_run(ctx, &op->phase, STEP_CB(op->cb), f->m->func_obj, JS_UNDEFINED, 0, NULL,
                          JS_UNDEFINED, &op->res, out_cb, out_argc);
        DCHECK(r == JS_STEP_CALL,
               "a module body's CALL request answered something other than a call — its cursor was already in "
               "flight, so the answer to the old request is about to be collected as this one's");
        return r;
    }
 popped:
    f = &op->frames[op->depth - 1];
    op->depth--;
    if (op->depth == 0) {
        js_module_eval_normal(ctx, op);
        return 0;
    }
    if (js_module_eval_after_child(ctx, op->frames[op->depth - 1].m, f->m, &result) < 0)
        goto abrupt;
    return JS_STEP_YIELD;
 abrupt:
    js_module_eval_abrupt(ctx, op, result);
    return 0;
}

static void step_module_evaluate_visit(JSContext *ctx, JSModuleEvalOp *op, JSStepVisit *v)
{
    int k;
    for (k = 0; k < 2; k++)
        v->val(ctx, &op->cb[k]);
    v->val(ctx, &op->res);
    v->buf(ctx, (void **)&op->frames, (size_t)op->cap * sizeof(*op->frames));
}

/* Run the <eval> function of the module and of all its requested
   modules. Return a promise or an exception. */
/* THE MODULE'S EVALUATION STATE, as one owned blob — the module twin of JS_AsyncStateSave, and for the same
   reason: it is shared baseline state that a flow WRITES, through fields no property hook can see. These are
   exactly 16.2.1.6.1.3.1's per-evaluation fields (the struct already groups them under "temp use during
   js_module_evaluate") plus the capability and the status they drive. The module's BINDINGS need nothing here —
   they are closure cells and cell_write already captures them; what was missing is the state that decides
   whether a flow evaluates AT ALL. */
typedef struct JSModuleEvalState {
    JSModuleStatus status;
    bool eval_started;
    int dfs_index, dfs_ancestor_index;
    JSModuleDef *stack_prev;
    int pending_async_dependencies;
    bool async_evaluation;
    int64_t async_evaluation_timestamp;
    JSModuleDef *cycle_root;
    bool eval_has_exception;
    JSValue promise;
    JSValue resolving_funcs[2];
    JSValue eval_exception;
    JSValue meta_obj;   /* §13.3.12.1's [[ImportMeta]]: written once per timeline, by JS_GetImportMeta */
    /* THE ASYNC PARENT LIST, and it has to be here because pending_async_dependencies is. The two are one
       accounting: js_module_eval_after_child increments the counter and appends to this array in the same
       breath, one pair per graph edge, and GatherAvailableAncestors decrements once per entry it walks.
       Capturing the counter WITHOUT the array is what broke: a flow saved at count 0, evaluated until the
       array held two parents and the counter read two, then restored the counter to zero while the array kept
       both entries — and the next walk decremented a counter that was already zero. That fired as
       "pending_async_dependencies > 0" and was invisible for as long as it was a plain assert compiled out by
       -DNDEBUG. The entries are borrowed JSModuleDef pointers owned by the module list, so the ARRAY is owned
       here and its contents are a shallow copy. */
    JSModuleDef **async_parent_modules;
    int async_parent_modules_count;
} JSModuleEvalState;

/* One copy helper, because the obligation is at four sites — save, restore, clone, free — and a field added to
   a struct that is copied field-by-field is exactly the shape that gets missed at one of them. */
static JSModuleDef **js_module_apm_dup(JSContext *ctx, JSModuleDef **src, int count)
{
    JSModuleDef **dst;
    if (count <= 0)
        return NULL;
    dst = js_malloc(ctx, sizeof(*dst) * count);
    if (!dst)
        return NULL;
    memcpy(dst, src, sizeof(*dst) * count);
    return dst;
}

void *JS_ModuleEvalStateSave(JSContext *ctx, void *mod)
{
    JSModuleDef *m = mod;
    JSModuleEvalState *b = js_malloc(ctx, sizeof(*b));
    if (!b)
        return NULL;
    /* The array is copied BEFORE any js_dup, so this failure path owns nothing but the blob. */
    b->async_parent_modules_count = m->async_parent_modules_count;
    b->async_parent_modules = js_module_apm_dup(ctx, m->async_parent_modules,
                                                m->async_parent_modules_count);
    if (m->async_parent_modules_count > 0 && !b->async_parent_modules) {
        js_free(ctx, b);
        return NULL;
    }
    b->status = m->status;
    b->eval_started = m->eval_started;
    b->dfs_index = m->dfs_index;
    b->dfs_ancestor_index = m->dfs_ancestor_index;
    b->stack_prev = m->stack_prev;
    b->pending_async_dependencies = m->pending_async_dependencies;
    b->async_evaluation = m->async_evaluation;
    b->async_evaluation_timestamp = m->async_evaluation_timestamp;
    b->cycle_root = m->cycle_root;
    b->eval_has_exception = m->eval_has_exception;
    b->promise = js_dup(m->promise);
    b->resolving_funcs[0] = js_dup(m->resolving_funcs[0]);
    b->resolving_funcs[1] = js_dup(m->resolving_funcs[1]);
    b->eval_exception = js_dup(m->eval_exception);
    b->meta_obj = js_dup(m->meta_obj);
    return b;
}

void JS_ModuleEvalStateRestore(JSContext *ctx, void *mod, void *blob)
{
    JSModuleDef *m = mod;
    JSModuleEvalState *b = blob;
    if (!b)
        return;
    JS_FreeValue(ctx, m->promise);
    JS_FreeValue(ctx, m->resolving_funcs[0]);
    JS_FreeValue(ctx, m->resolving_funcs[1]);
    JS_FreeValue(ctx, m->eval_exception);
    JS_FreeValue(ctx, m->meta_obj);
    /* NOT `js_module_set_status`, AND THIS IS THE ONE SITE WHERE THAT IS THE DEFECT RATHER THAN THE FIX. A
       restore is the delta being UNAPPLIED, so announcing it would offer the value being restored as this
       flow's FIRST baseline — and the host's dedup is first-baseline-wins, so the announcement would pin the
       undo as the thing to undo to. A sweep over this write's spelling that routed it would invert the
       mechanism silently, every status write still going through one helper and every restore recording the
       wrong baseline. */
    m->status = b->status;
    m->eval_started = b->eval_started;
    m->dfs_index = b->dfs_index;
    m->dfs_ancestor_index = b->dfs_ancestor_index;
    m->stack_prev = b->stack_prev;
    m->pending_async_dependencies = b->pending_async_dependencies;
    m->async_evaluation = b->async_evaluation;
    m->async_evaluation_timestamp = b->async_evaluation_timestamp;
    m->cycle_root = b->cycle_root;
    m->eval_has_exception = b->eval_has_exception;
    m->promise = js_dup(b->promise);
    m->resolving_funcs[0] = js_dup(b->resolving_funcs[0]);
    m->resolving_funcs[1] = js_dup(b->resolving_funcs[1]);
    m->eval_exception = js_dup(b->eval_exception);
    m->meta_obj = js_dup(b->meta_obj);
    /* The array is restored WITH the counter, never without it — see JSModuleEvalState. */
    js_free(ctx, m->async_parent_modules);
    m->async_parent_modules = js_module_apm_dup(ctx, b->async_parent_modules,
                                                b->async_parent_modules_count);
    CHECK(b->async_parent_modules_count <= 0 || m->async_parent_modules != NULL,
          "out of memory restoring a module's async parent list — the flow's accounting cannot be made whole");
    m->async_parent_modules_count = b->async_parent_modules_count;
    m->async_parent_modules_size = b->async_parent_modules_count;
}

void *JS_ModuleEvalStateClone(JSContext *ctx, void *blob)
{
    JSModuleEvalState *b = blob, *c;
    if (!b)
        return NULL;
    c = js_malloc(ctx, sizeof(*c));
    if (!c)
        return NULL;
    *c = *b;
    c->async_parent_modules = js_module_apm_dup(ctx, b->async_parent_modules,
                                                b->async_parent_modules_count);
    if (b->async_parent_modules_count > 0 && !c->async_parent_modules) {
        js_free(ctx, c);
        return NULL;
    }
    c->promise = js_dup(b->promise);
    c->resolving_funcs[0] = js_dup(b->resolving_funcs[0]);
    c->resolving_funcs[1] = js_dup(b->resolving_funcs[1]);
    c->eval_exception = js_dup(b->eval_exception);
    c->meta_obj = js_dup(b->meta_obj);
    return c;
}

int JS_ModuleEvalStateStatus(const void *blob)
{
    const JSModuleEvalState *b = blob;

    DCHECK(b != NULL, "a module's [[Status]] was read from no saved state");
    _Static_assert(JS_MODULE_EVAL_UNLINKED == JS_MODULE_STATUS_UNLINKED &&
                   JS_MODULE_EVAL_LINKING == JS_MODULE_STATUS_LINKING &&
                   JS_MODULE_EVAL_LINKED == JS_MODULE_STATUS_LINKED &&
                   JS_MODULE_EVAL_EVALUATING == JS_MODULE_STATUS_EVALUATING &&
                   JS_MODULE_EVAL_EVALUATING_ASYNC == JS_MODULE_STATUS_EVALUATING_ASYNC &&
                   JS_MODULE_EVAL_EVALUATED == JS_MODULE_STATUS_EVALUATED,
                   "the exported [[Status]] values must be the engine's own");
    return (int)b->status;
}

void JS_ModuleEvalStateFree(JSRuntime *rt, void *blob)
{
    JSModuleEvalState *b = blob;
    if (!b)
        return;
    JS_FreeValueRT(rt, b->promise);
    JS_FreeValueRT(rt, b->resolving_funcs[0]);
    JS_FreeValueRT(rt, b->resolving_funcs[1]);
    JS_FreeValueRT(rt, b->eval_exception);
    JS_FreeValueRT(rt, b->meta_obj);
    js_free_rt(rt, b->async_parent_modules);
    js_free_rt(rt, b);
}

/* Announce that this flow is about to change `m`'s evaluation state. Fired at every point that writes one of the
   captured fields; the host captures the FIRST one per flow and ignores the rest.
   AND THAT SENTENCE IS A CONTRACT RATHER THAN A DESCRIPTION, SO HERE IS WHAT MAKES IT TRUE — it was FALSE for
   the whole of §16.2.1.6.1.2 Link ( ), which writes `status`, `dfs_index`, `dfs_ancestor_index` and
   `stack_prev` and called this from nowhere. EVERY STATUS WRITE IN EITHER PHASE NOW GOES THROUGH
   `js_module_set_status`, which announces before it writes, so the contract is structural for the one field
   both phases share and a reader does not have to count call sites to believe it.
   THE RETIRED WORDING IS KEPT BECAUSE IT IS WHAT A READER RE-DERIVES FROM COUNTING THOSE CALLS. It read: the
   two phases are covered two different ways and the difference is worth stating, because a reader counting
   CALL SITES gets the wrong answer for one of them — the EVAL phase by a call per site at its entries, each
   stating its own reason, and the LINK phase by js_module_link_set_status, which is the only way that phase
   spells a status write, so the announcement is structural there rather than remembered. That was exact, and
   the asymmetry it describes is the thing that went: the eval phase's six bare writes are routed and the
   link-specific NAME is gone, because two identical bodies under two names is the spelling that drifts.
   WHAT A CALL COUNT STILL CANNOT TELL YOU is the other three fields, and that is the paragraph below: they
   are covered by a property of the WALK rather than by a write-site helper, because they are not status
   writes and no shared setter spells them.
   FIVE link-phase writes reach a field this blob holds and only THREE of them are status writes: step 11's pop,
   the pre-order claim, step 4's reset, AND the two `dfs_ancestor_index` minima in js_module_link_advance, which
   every enumeration of this gap has missed because they are not status writes and do not look like state. All
   five are covered by ONE announcement, and by a property of the walk rather than by a call at each: a module
   reaches the SCC stack or a frame on the walk's path only through js_module_linking_enter, which announces
   before it claims, and the host's dedup is first-baseline-wins per flow. So the two minima are the same flow
   writing a record it has already announced — not an omission, and not a site to add a call to.
   WHAT WAS SAID TO BE STILL REMEMBERED RATHER THAN STRUCTURAL IS RETIRED, AND IS KEPT IN ITS OWN WORDS BECAUSE
   A READER COUNTING CALL SITES RE-DERIVES IT, AND IT IS GIVEN UNQUOTED BECAUSE A QUOTED RUN THAT IS THIS TREE'S
   OWN PROSE IS COMPARED AGAINST WHATEVER STANDARD THE NEAREST PRECEDING CITATION NAMES — measured: quoted, the
   citation audit reported this retired clause as a divergence from ECMAScript §16.2.1.6.1.2, which is a false
   accusation produced by punctuation and not by anything about the text. It read — the eval phase: its status
   writes do not go through a helper, so a site added there can still forget. That is the next construction, not
   this one. The paragraph
   two up says the opposite about the same fact, so this block contradicted itself and a reader reaching the end
   of it got the wrong half. MEASURED WITH THE COMMAND: `grep -nE '\bm1?->status *= *[^=]'` over this file
   answers TWO — js_module_set_status's own line, and JS_ModuleEvalStateRestore's, which the note above
   deliberately exempts — against 38 for the `==` spelling as the armed control, so no status write in either
   phase is remembered. What is still covered by a property of the WALK rather than by a write-site helper is
   the three non-status fields of the paragraph above, and nothing else. */
static void js_module_eval_capture(JSContext *ctx, JSModuleDef *m)
{
    if (g_time_travel.module_eval)
        g_time_travel.module_eval(ctx, m);
}

/* THE MODULE'S EVALUATION CAPABILITY, created WITHOUT evaluating. A consumer has to be able to register its
   continuation on it BEFORE the body runs: the body can FORK, and a sibling forked mid-body inherits the world
   as it was at the branch, where a registration made after evaluation returned never happened. A promise may be
   created only on the cycle_root of a cycle. Returns a dup, or JS_EXCEPTION on allocation failure. */
static JSValue js_module_eval_capability(JSContext *ctx, JSModuleDef *m)
{
    js_module_eval_capture(ctx, m);
    if (m->status == JS_MODULE_STATUS_EVALUATING_ASYNC ||
        m->status == JS_MODULE_STATUS_EVALUATED)
        m = m->cycle_root;
    if (JS_IsUndefined(m->promise)) {
        m->promise = JS_NewPromiseCapability(ctx, m->resolving_funcs);
        if (JS_IsException(m->promise))
            return JS_EXCEPTION;
    }
    return js_dup(m->promise);
}

