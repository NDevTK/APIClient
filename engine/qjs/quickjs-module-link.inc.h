/* THE LOAD IS NOT HERE ANY MORE. js_resolve_module stood at this point and did two of the spec's three
   phases at once: it WALKED the graph and it asked the host for every child it did not have, from inside a
   synchronous C walk with no point to suspend at — and it ran at COMPILE time as well, which 16.2.1.7.1
   ParseModule does not do (a parsed record's [[LoadedModules]] is empty and stays empty until a load).
   16.2.1.6.1.1 LoadRequestedModules is that phase, it is a PROMISE, and it lives beside the other module
   operations it belongs with (js_module_load_requested). Linking below now reaches every child through
   rme->module alone, which is 16.2.1.9 GetImportedModule — one step, and that step is an assertion. */

static JSVarRef *js_create_module_var(JSContext *ctx, bool is_lexical)
{
    JSVarRef *var_ref;
    var_ref = js_malloc(ctx, sizeof(JSVarRef));
    if (!var_ref)
        return NULL;
    JS_REF_COUNT(var_ref) = 1;
    if (is_lexical)
        var_ref->value = JS_UNINITIALIZED;
    else
        var_ref->value = JS_UNDEFINED;
    var_ref->pvalue = &var_ref->value;
    var_ref->is_detached = true;
    var_ref->is_coro = false;   /* a module binding aliases no frame, so there is no owner to root */
    var_ref->flow_gen = g_flow_gen;
    add_gc_object(ctx->rt, &var_ref->header, JS_GC_OBJ_TYPE_VAR_REF);
    return var_ref;
}

/* Create the <eval> function associated with the module */
static int js_create_module_bytecode_function(JSContext *ctx, JSModuleDef *m)
{
    JSFunctionBytecode *b;
    int i;
    JSVarRef **var_refs;
    JSValue func_obj, bfunc;
    JSObject *p;

    bfunc = m->func_obj;
    b = JS_VALUE_GET_PTR(bfunc);
    /* The object's class is its bytecode's kind, as js_closure makes it: every call shape dispatches on the
       class, so a module body (always JS_FUNC_ASYNC) called as a plain bytecode function runs its
       OP_return_async with no async activation to settle and unwinds its caller's frame instead. */
    DCHECK(b->func_kind == JS_FUNC_ASYNC,
           "a Source Text Module Record's body was compiled with a kind other than async — "
           "js_parse_program compiles every module body as an async function");
    func_obj = JS_NewObjectProtoClass(ctx, ctx->function_proto,
                                      func_kind_to_class_id[b->func_kind]);

    if (JS_IsException(func_obj))
        return -1;

    p = JS_VALUE_GET_OBJ(func_obj);
    p->u.func.function_bytecode = b;
    JS_REF_COUNT(b)++;
    p->u.func.home_object = NULL;
    p->u.func.var_refs = NULL;
    /* A MODULE'S OWN FUNCTION OBJECT IS NOT A CLOSURE THE PAGE CREATED, so it takes no creation ordinal: it is
       built once per module by the loader rather than by a body the page ran, and JS_CreationName's 0 answers
       ABSENT for it. Written rather than left alone because the allocation does not zero this field. */
    p->u.func.creation_ord = 0;
    p->u.func.timeline = js_timeline_handle();
    if (b->closure_var_count) {
        var_refs = js_mallocz(ctx, sizeof(var_refs[0]) * b->closure_var_count);
        if (!var_refs)
            goto fail;
        p->u.func.var_refs = var_refs;

        /* create the global variables. The other variables are
           imported from other modules */
        for(i = 0; i < b->closure_var_count; i++) {
            JSClosureVar *cv = &b->closure_var[i];
            JSVarRef *var_ref;
            if (cv->closure_type == JS_CLOSURE_MODULE_DECL) {
                var_ref = js_create_module_var(ctx, cv->is_lexical);
                if (!var_ref)
                    goto fail;

                module_trace(ctx, "local %d: %p\n", i, (void *)var_ref);

                var_refs[i] = var_ref;
            }
        }
    }
    m->func_obj = func_obj;
    JS_FreeValue(ctx, bfunc);
    return 0;
 fail:
    JS_FreeValue(ctx, func_obj);
    return -1;
}

/* must be done before 16.2.1.6.1.2 Link ( ) because of cyclic references */
/* Create ONE module's function and exported variable slots. The walk over the dependency graph is
   js_create_module_function below; this half creates nothing recursively and runs no user code. */
static int js_create_module_function_one(JSContext *ctx, JSModuleDef *m)
{
    bool is_c_module;
    int i;
    JSVarRef *var_ref;

    is_c_module = (m->init_func != NULL);

    if (is_c_module) {
        /* initialize the exported variables */
        for(i = 0; i < m->export_entries_count; i++) {
            JSExportEntry *me = &m->export_entries[i];
            if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
                var_ref = js_create_module_var(ctx, false);
                if (!var_ref)
                    return -1;
                me->u.local.var_ref = var_ref;
            }
        }
    } else {
        if (js_create_module_bytecode_function(ctx, m))
            return -1;
    }
    return 0;
}

/* The dependency walk, as an explicit stack. It recursed with NO overflow guard at all, so a deep module graph did
   not even get an error: 32649 frames of js_create_module_function and then a SEGFAULT inside malloc. Children are
   pushed in reverse so the visit order is the recursion's pre-order exactly; nothing here runs user code, so the
   order is not observable, but a conversion that changes what it does not have to is harder to trust. */
static int js_create_module_function(JSContext *ctx, JSModuleDef *m)
{
    JSModuleDef **stack = NULL;
    int depth = 0, cap = 0, ret = -1, i;

    if (m->func_created)
        return 0;
    for (;;) {
        if (js_create_module_function_one(ctx, m) < 0)
            goto done;
        m->func_created = true;
        if (depth + m->req_module_entries_count > cap) {
            int ncap = cap ? cap * 2 : 16;
            JSModuleDef **ns;
            while (ncap < depth + m->req_module_entries_count)
                ncap *= 2;
            ns = js_realloc(ctx, stack, sizeof(*ns) * (size_t)ncap);
            if (unlikely(!ns))
                goto done;
            stack = ns;
            cap = ncap;
        }
        for (i = m->req_module_entries_count - 1; i >= 0; i--) {
            /* 16.2.1.9 GetImportedModule, at the EARLIEST reader of [[LoadedModules]] — this walk runs before
               Link and would dereference the NULL rather than assert on it. A request with no record here is
               the load phase not having run (or not having finished) on this graph. */
            DCHECK(m->req_module_entries[i].module != NULL,
                   "16.2.1.9: a module request had no loaded record when its graph's functions were created — "
                   "js_module_load_requested has not completed on this graph");
            stack[depth++] = m->req_module_entries[i].module;
        }
        do {
            if (depth == 0) { ret = 0; goto done; }
            m = stack[--depth];
        } while (m->func_created);
    }
 done:
    js_free(ctx, stack);
    return ret;
}


/* 16.2.1.6.1.2.1 InnerModuleLinking steps 9-10 for ONE module: the indirect-export check and the import
   resolution — everything 16.2.1.7.3.1 InitializeEnvironment performs BEFORE its declaration instantiation.
   Nothing here runs the page's code: every operation the clause names is `!` or a walk over the module graph,
   which is what makes this half a plain C function while its successor is a CALL.
   It is its own function because the WALK around it is no longer C recursion, and because the caller has to be
   able to STOP between it and the prologue — see js_module_link_advance below.
   IT RUNS ONCE PER FLOW THAT LINKS `m`, not once per record: a module's status is per flow (the COW delta's
   module_eval entry), so a sibling forked before the link links again. The cells it wires are shared baseline
   state and resolve to the same cells every time, so a slot already wired keeps the one counted reference it
   holds. */
static int js_module_linking_bindings(JSContext *ctx, JSModuleDef *m)
{
    int i;
    JSImportEntry *mi;
    JSModuleDef *m1;
    JSVarRef **var_refs, *var_ref;
    JSObject *p;
    bool is_c_module;

#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
    if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
        char buf1[ATOM_GET_STR_BUF_SIZE];
        printf("instantiating module '%s':\n", JS_AtomGetStr(ctx, buf1, sizeof(buf1), m->module_name));
    }
#endif
    /* check the indirect exports */
    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        if (me->export_type == JS_EXPORT_TYPE_INDIRECT &&
            !me->is_namespace) {
            JSResolveResultEnum ret;
            JSExportEntry *res_me;
            JSModuleDef *res_m, *m1;
            m1 = m->req_module_entries[me->u.req_module_idx].module;
            ret = js_resolve_export(ctx, &res_m, &res_me, m1, me->local_name);
            if (ret != JS_RESOLVE_RES_FOUND) {
                /* the binding is missing from the re-exported module m1, not
                   from the re-exporting module m; report m1 and the name being
                   looked up there, matching the direct-import diagnostic */
                js_resolve_export_throw_error(ctx, ret, m1, me->local_name);
                goto fail;
            }
        }
    }

#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
    if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
        printf("exported bindings:\n");
        for(i = 0; i < m->export_entries_count; i++) {
            JSExportEntry *me = &m->export_entries[i];
            printf(" name="); print_atom(ctx, me->export_name);
            printf(" local="); print_atom(ctx, me->local_name);
            printf(" type=%d idx=%d\n", me->export_type, me->u.local.var_idx);
        }
    }
#endif

    is_c_module = (m->init_func != NULL);

    if (!is_c_module) {
        p = JS_VALUE_GET_OBJ(m->func_obj);
        var_refs = p->u.func.var_refs;

        for(i = 0; i < m->import_entries_count; i++) {
            mi = &m->import_entries[i];
#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
            if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
                printf("import var_idx=%d name=", mi->var_idx);
                print_atom(ctx, mi->import_name);
                printf(": ");
            }
#endif
            m1 = m->req_module_entries[mi->req_module_idx].module;
            if (mi->is_namespace) {
                JSValue val;
                /* name space import */
                val = JS_GetModuleNamespace(ctx, m1);
                if (JS_IsException(val))
                    goto fail;
                set_value(ctx, &var_refs[mi->var_idx]->value, val);

                module_trace(ctx, "namespace\n");

            } else {
                JSResolveResultEnum ret;
                JSExportEntry *res_me;
                JSModuleDef *res_m;

                ret = js_resolve_export(ctx, &res_m,
                                        &res_me, m1, mi->import_name);
                if (ret != JS_RESOLVE_RES_FOUND) {
                    js_resolve_export_throw_error(ctx, ret, m1, mi->import_name);
                    goto fail;
                }
                if (res_me->is_namespace) {
                    JSValue val;
                    JSModuleDef *m2;
                    /* name space import from */
                    m2 = res_m->req_module_entries[res_me->u.req_module_idx].module;
                    val = JS_GetModuleNamespace(ctx, m2);
                    if (JS_IsException(val))
                        goto fail;
                    /* A slot an earlier link wired keeps its cell: the namespace is m2's one object. */
                    var_ref = var_refs[mi->var_idx];
                    if (!var_ref) {
                        var_ref = js_create_module_var(ctx, true);
                        if (!var_ref) {
                            JS_FreeValue(ctx, val);
                            goto fail;
                        }
                        var_refs[mi->var_idx] = var_ref;
                    }
                    set_value(ctx, &var_ref->value, val);

                    module_trace(ctx, "namespace from\n");

                } else {
                    /* follow the import alias when the slot is NULL (cyclic
                       re-export of an imported binding) */
                    var_ref = js_get_local_export_var_ref(ctx, res_m, res_me);
                    if (!var_ref) {
                        char buf2[ATOM_GET_STR_BUF_SIZE];
                        char buf3[ATOM_GET_STR_BUF_SIZE];
                        JS_ThrowSyntaxError(ctx,
                            "circular import: binding '%s' is not resolvable in module '%s'",
                            JS_AtomGetStr(ctx, buf2, sizeof(buf2), mi->import_name),
                            JS_AtomGetStr(ctx, buf3, sizeof(buf3), res_m->module_name));
                        goto fail;
                    }
                    DCHECK(var_refs[mi->var_idx] == NULL || var_refs[mi->var_idx] == var_ref,
                           "a module's import slot is wired to a different cell than this link resolves it to — "
                           "the exporting module's cells are shared baseline state, so every flow's link "
                           "resolves one import to one cell");
                    if (!var_refs[mi->var_idx]) {
                        JS_REF_COUNT(var_ref)++;
                        var_refs[mi->var_idx] = var_ref;
                    }

                    module_trace(ctx, "local export (var_ref=%p)\n", (void *)var_ref);
                }
            }
        }

        /* keep the exported variables in the module export entries (they
           are used when the eval function is deleted and cannot be
           initialized before in case imports are exported) */
        for(i = 0; i < m->export_entries_count; i++) {
            JSExportEntry *me = &m->export_entries[i];
            if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
                var_ref = var_refs[me->u.local.var_idx];
                DCHECK(me->u.local.var_ref == NULL || me->u.local.var_ref == var_ref,
                       "a module's export entry holds a different cell than its own declaration's — the "
                       "entry is written only here, from the module function's cell for that binding");
                if (!me->u.local.var_ref) {
                    JS_REF_COUNT(var_ref)++;
                    me->u.local.var_ref = var_ref;
                }
            }
        }

    }
    /* THE PROLOGUE IS NOT HERE, and its absence is the whole of this split. 16.2.1.7.3.1 InitializeEnvironment's
       steps 32-37 — the var and lexical declaration instantiation, and the InstantiateFunctionObject each
       function declaration needs — are compiled into the module body's own `OP_push_this; OP_if_false <body>`
       prologue, so performing them is ENTERING A BYTECODE BODY. That is a call, and in this engine a call the
       page's flow may be parked inside; the caller issues it (js_module_link_advance's MODLINK_PROLOGUE) so
       that the frame it pushes lands on the RUNNING flow's chain rather than on a second base of its own. */
    return 0;
 fail:
    return -1;
}

static void js_module_eval_capture(JSContext *ctx, JSModuleDef *m);

/* EVERY WRITE OF A MODULE'S STATUS, IN EITHER PHASE, AND THE CAPTURE IS PART OF THE WRITE RATHER THAN A CALL
   BESIDE IT. `JSModuleEvalState` holds `status` among its fields, and §16.2.1.6.1.2 Link ( ) and
   §16.2.1.6.1.3 Evaluate ( ) both write it — so a status write that does not announce itself is BASELINE
   state every sibling flow sees, while the §16.2.1.7.3.1 prologue those writes lead to writes its cells
   through `OP_put_var_ref`, which IS captured per flow. That is one phase isolated in two opposite
   directions: every world reads the module LINKED and only the linking world ran the prologue, so a sibling
   reads a top-level FUNCTION DECLARATION's cell at JS_UNINITIALIZED and throws `<name> is not initialized` on
   a binding the module's own code needs. No amount of re-evaluating reaches it: §16.2.1.6.1.3 Evaluate ( )
   enters the body with `this` UNDEFINED, so `OP_push_this; OP_if_false` takes the false arm and the prologue
   is skipped. That is the defect this helper was built for, in the LINK phase, and the reasoning is kept
   because it is what a reader re-derives.
   IT IS A HELPER SO THE CAPTURE CANNOT BE THE LINE A LATER SITE FORGETS, which is the one thing a call beside
   each write does not buy: a status write is unspellable without its announcement, so the contract
   `js_module_eval_capture` states about itself — fired at every point that writes one of the captured fields
   — is true by CONSTRUCTION and not by an argument about a call graph.
   AND IT IS ONE HELPER AND NOT TWO, WHICH IS A CORRECTION TO ITS OWN EARLIER NAME. It was
   `js_module_link_set_status` and the eval phase wrote `m->status` bare at six sites, so the two phases were
   covered two different ways — one structurally, one by a call per site that a later site could forget. The
   obvious repair is a second helper for the eval phase, and that is two identical bodies under two names,
   which is the two-right-answers-to-one-question shape: the phase-specific NAME is what would drift, because
   nothing makes a new site pick the one its phase is spelled with. So the phase lives in this comment and the
   SPELLING is shared, and the superseded name is gone rather than kept beside it.
   THE EVAL PHASE'S OLD COVERAGE RESTED ON AN ARGUMENT ABOUT THE WALK, NOT ON ITS CALLS, which is the part
   worth keeping because it was SOUND and is the kind of soundness this helper exists to stop relying on.
   Three of those six writes are to `m1` — a module OTHER than the one the enclosing entry captured — inside
   §16.2.1.6.1.3.1's SCC pop and its abrupt-completion unwind, and they were covered because a module reaches
   that stack only through `js_module_eval_enter`, which announces before it claims, and the host's dedup is
   first-baseline-wins per flow. Correct, and a property of the call graph: a fourth site writing an `m1` it
   had not entered would have been BASELINE with nothing to say so.
   WHAT IS DELIBERATELY NOT ROUTED is `JS_ModuleEvalStateRestore`, and the reason is stated at that site: a
   restore is the delta being UNAPPLIED, so announcing it would offer the value being restored as this flow's
   first baseline. A sweep over `->status =` that routes it has inverted the mechanism, which is why the note
   is there and not here.
   RETIREMENT: this record goes when the build REFUSES a bare module-status write outside this helper and the
   restore, so the shared spelling is enforced rather than conventional — MEASURED ABSENT with the command, so
   this condition is not born met: `grep -c moduleStatusWrite engine/build.mjs` answers 0 against
   `grep -c abiCheck engine/build.mjs` answering 4 as the armed control. */
static void js_module_set_status(JSContext *ctx, JSModuleDef *m, JSModuleStatus status)
{
    js_module_eval_capture(ctx, m);
    m->status = status;
}

/* 16.2.1.6.1.2.1 InnerModuleLinking step 11: the SCC pop. `m` is the root of a strongly connected component
   exactly when its ancestor index never moved, and every module above it on `stack` linked with it. */
static void js_module_linking_scc_pop(JSContext *ctx, JSModuleDef *m, JSModuleDef **pstack_top)
{
    JSModuleDef *m1;

    DCHECK(m->dfs_ancestor_index <= m->dfs_index, "m->dfs_ancestor_index <= m->dfs_index");
    if (m->dfs_index != m->dfs_ancestor_index)
        return;
    for(;;) {
        /* pop m1 from stack */
        m1 = *pstack_top;
        DCHECK(m1 != NULL,
               "16.2.1.6.1.2.1 step 11.b: the SCC stack ran out before its own root — the walk popped a "
               "component whose members are no longer the ones step 8 appended");
        *pstack_top = m1->stack_prev;
        js_module_set_status(ctx, m1, JS_MODULE_STATUS_LINKED);
        if (m1 == m)
            break;
    }
}

/* 16.2.1.6.1.2 Link ( ) AS A RECORD A FLOW CAN BE PARKED INSIDE — 16.2.1.6.1.2.1 InnerModuleLinking's depth-first
   traversal with its cursor on the HEAP, and step 2's `stack` beside it, so that neither lives on the C stack.
     THE DEPTH came first and is the smaller half. The recursion opened with js_check_stack_overflow and a
   JS_ThrowStackOverflow, so a module graph deeper than the C stack failed with a synthetic error — a BOUND on
   the input, which this engine does not have, and a graph is data, so its depth is the page's to choose. The
   frame array grows; nothing caps it.
     THE SUSPENSION is the reason the cursor is a record rather than a local. Step 9's InitializeEnvironment ends
   in the module body's `OP_push_this; OP_if_false` prologue, and entering a bytecode body is entering the
   INTERPRETER, whose yield poll runs at EVERY opcode dispatch — so a flow can be asked for the thread inside a
   prologue exactly as it can inside any other body. It has nothing to do with the prologue's SHAPE: a comment
   here used to argue that the prologue is compiled with no back-edge and therefore "nothing in it can preempt",
   and that was never true of this interpreter. A back-edge is one of the places a request is RAISED; the request
   is also raised by things that are not the flow's instruction stream (the dispatch-counted budget,
   frontier_rank_changed), and it is ANSWERED wherever the next opcode is fetched. Straight-line code parks.
     WHICH IS NOT LINKING BECOMING ASYNCHRONOUS, and the standard is what settles that. 16.2.1.6.1.2 Link ( )
   returns a normal completion containing unused or a throw completion — synchronously, unlike 16.2.1.6.1.3
   Evaluate ( ), which returns a Promise — and every operation 16.2.1.7.3.1 InitializeEnvironment performs is `!`
   or a walk of the module graph: ResolveExport, NewModuleEnvironment, CreateImmutableBinding,
   CreateImportBinding, InstantiateFunctionObject. Not one of them runs the page's code, so NOTHING OBSERVABLE
   may happen between the steps of a link, and nothing does: a scheduler park runs no page code in this
   timeline — a sibling flow is a sibling WORLD, isolated by its own COW delta — and the resume is byte-identical.
   Parkable and asynchronous are different properties, and only the second one the standard forbids.
     The traversal is Tarjan's, unchanged: `index` and the dfs/ancestor indices are threaded exactly as the
   recursion threaded them, and the parent's ancestor-index minimum is taken after a child is entered (the
   already-visited case) or finished (the recursed case), which is where the recursive form took it. */
typedef struct JSModuleLinkFrame {
    JSModuleDef *m;
    int i;              /* cursor over m->req_module_entries: which child comes next */
} JSModuleLinkFrame;

/* WHERE THE NEXT ADVANCE CONTINUES. A resume point, not a phase counter: each names a place the walk STOPPED,
   and a walk that stopped nowhere else can be entered at no other. */
enum { MLW_DESCEND,    /* inside frames[depth-1]'s child loop (step 9's `For each ModuleRequest Record request`) */
       MLW_POP,        /* frames[depth-1]'s steps 9-10 are done, including its prologue: step 11 comes next */
       MLW_DONE };     /* step 12: the root has finished, or never needed a visit */

typedef struct JSModuleLinkWalk {
    JSModuleLinkFrame *frames;   /* OWNED, and declared to the step teardown by the machine's `visit` */
    int depth, cap;
    int index;                   /* 16.2.1.6.1.2.1's `index` */
    JSModuleDef *stack_top;      /* 16.2.1.6.1.2 step 2's `stack`, threaded through JSModuleDef.stack_prev */
    uint8_t resume;
} JSModuleLinkWalk;

/* what js_module_link_advance answers */
enum { MODLINK_DONE = 0,      /* 16.2.1.6.1.2 step 5: the graph is linked */
       MODLINK_MORE,          /* a rest point with nothing to run: advance again when the scheduler says so */
       MODLINK_PROLOGUE };    /* *pm's hoisted-definition prologue must be CALLED, then advance again */

/* the PRE-ORDER half (steps 3-7): claim the module, number it, push it on the SCC stack. Returns the new index, or
   -1 when the module needs no visit at all — which is the recursive form's early `return index`. */
static int js_module_linking_enter(JSContext *ctx, JSModuleDef *m, JSModuleDef **pstack_top, int index)
{
    /* BEFORE THE TEST AND NOT ONLY BEFORE THE WRITE, for the reason step_module_evaluate_run gives at its own entry:
       whether this flow LINKS the module at all is decided by a status a sibling may have written, so the four
       statuses below are read in this flow's world or the decision is made in somebody else's. The early return
       is the half that was costing the most — a sibling reading LINKING or LINKED skips the module entirely and
       then evaluates it, so its §16.2.1.7.3.1 prologue never runs in that world and its top-level function
       declarations stay JS_UNINITIALIZED. */
    js_module_eval_capture(ctx, m);
    if (m->status == JS_MODULE_STATUS_LINKING ||
        m->status == JS_MODULE_STATUS_LINKED ||
        m->status == JS_MODULE_STATUS_EVALUATING_ASYNC ||
        m->status == JS_MODULE_STATUS_EVALUATED)
        return -1;
    DCHECK(m->status == JS_MODULE_STATUS_UNLINKED, "m->status == JS_MODULE_STATUS_UNLINKED");
    /* THE ONLY PUSH ONTO THE WALK'S SCC STACK, which is what makes the other four link-phase writes to a
       captured field covered by THIS capture: step 11's pop, step 4's reset and the two `dfs_ancestor_index`
       minima in js_module_link_advance all name a module that is on this walk's stack or has a frame on its
       path, and a module gets to either one only through this line. The capture dedups first-baseline-wins per
       flow, so the value recorded here is the pre-LINKING one and those four writes need no announcement of
       their own — they are the same flow writing a record it has already announced. */
    js_module_set_status(ctx, m, JS_MODULE_STATUS_LINKING);
    m->dfs_index = index;
    m->dfs_ancestor_index = index;
    /* push 'm' on stack */
    m->stack_prev = *pstack_top;
    *pstack_top = m;
    return index + 1;
}

static int js_module_link_push(JSContext *ctx, JSModuleLinkWalk *w, JSModuleDef *m)
{
    if (w->depth == w->cap) {
        int ncap = w->cap ? w->cap * 2 : 8;
        JSModuleLinkFrame *nf = js_realloc(ctx, w->frames, sizeof(*nf) * (size_t)ncap);
        if (unlikely(!nf))
            return -1;
        w->frames = nf;
        w->cap = ncap;
    }
    w->frames[w->depth].m = m;
    w->frames[w->depth].i = 0;
    w->depth++;
    return 0;
}

/* 16.2.1.6.1.2 Link ( ) steps 1-3's entry: the root's pre-order half, or the early return when it needs none. */
static int js_module_link_init(JSContext *ctx, JSModuleLinkWalk *w, JSModuleDef *m)
{
    int nindex;

    w->frames = NULL;
    w->depth = w->cap = 0;
    w->index = 0;
    w->stack_top = NULL;      /* step 2: `stack` is a new empty List */
    w->resume = MLW_DONE;

#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
    if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
        char buf1[ATOM_GET_STR_BUF_SIZE];
        printf("js_link_module '%s':\n", JS_AtomGetStr(ctx, buf1, sizeof(buf1), m->module_name));
    }
#endif
    /* step 1 */
    DCHECK(m->status == JS_MODULE_STATUS_UNLINKED || m->status == JS_MODULE_STATUS_LINKED ||
           m->status == JS_MODULE_STATUS_EVALUATING_ASYNC || m->status == JS_MODULE_STATUS_EVALUATED,
           "16.2.1.6.1.2 step 1: Link was entered on a module that is already LINKING — a second link of a "
           "graph whose first one has not finished, which the module map's memo is what prevents");
    nindex = js_module_linking_enter(ctx, m, &w->stack_top, w->index);
    if (nindex < 0)
        return 0;                          /* 16.2.1.6.1.2.1 step 2: already linked, so there is nothing to walk */
    w->index = nindex;
    if (js_module_link_push(ctx, w, m) < 0)
        return -1;
    w->resume = MLW_DESCEND;
    return 0;
}

/* ONE STEP OF THE WALK. It answers MODLINK_PROLOGUE with *pm set when the next thing 16.2.1.7.3.1 owes is the
   module body's declaration-instantiation entry — which is a CALL, and therefore the caller's to issue on the
   running flow's chain — and MODLINK_MORE at every other rest point, so a caller that is a step machine can
   offer the scheduler the thread between any two modules of the graph. -1 = threw. */
static int js_module_link_advance(JSContext *ctx, JSModuleLinkWalk *w, JSModuleDef **pm)
{
    JSModuleLinkFrame *f;
    int nindex;

    *pm = NULL;
    if (w->resume == MLW_DONE)
        return MODLINK_DONE;
    if (w->resume == MLW_POP)
        goto pop;

    DCHECK(w->resume == MLW_DESCEND,
           "a module link walk was advanced at a resume point it never stopped at — MLW_* names the three "
           "places the walk can be entered, and a fourth value is memory nothing in this file wrote");
    DCHECK(w->depth > 0,
           "a module link walk advanced with no frame on its path — js_module_link_init pushes the root's "
           "frame, and MLW_DONE is how a walk with nothing to do says so");
    f = &w->frames[w->depth - 1];
    while (f->i < f->m->req_module_entries_count) {
        JSModuleDef *m1 = f->m->req_module_entries[f->i].module;
        f->i++;
        /* 16.2.1.9 GetImportedModule, whose whole body is `Assert: records has exactly one element, since
           LoadRequestedModules has completed successfully on referrer prior to invoking this abstract
           operation`. Linking asks the host for NOTHING; a request with no record is a graph that was
           linked without being loaded, which is the phase order broken and not a module that is missing. */
        DCHECK(m1 != NULL,
               "16.2.1.9: a module request reached LINKING with no loaded record — Link never loads, so "
               "this graph was linked before js_module_load_requested finished on it (or at all)");
        nindex = js_module_linking_enter(ctx, m1, &w->stack_top, w->index);
        if (nindex < 0) {
            /* already visited: take the minimum the recursive form took on return */
            if (m1->status == JS_MODULE_STATUS_LINKING)
                f->m->dfs_ancestor_index = min_int(f->m->dfs_ancestor_index, m1->dfs_ancestor_index);
            continue;
        }
        w->index = nindex;
        if (js_module_link_push(ctx, w, m1) < 0)   /* descend: a frame for m1 goes on top */
            return -1;
        return MODLINK_MORE;
    }
    /* every child is done, so this module's post-order half runs: steps 9-10 first, and they run no page code */
    if (js_module_linking_bindings(ctx, f->m) < 0)
        return -1;
    w->resume = MLW_POP;
    /* …and then 16.2.1.7.3.1's declaration instantiation, which for a Source Text Module Record IS its body's
       hoisted-definition prologue. A module with an init_func is this engine's Synthetic Module Record shape:
       16.2.1.8.4.4 Link ( ) is `Return unused`, so there is no environment to initialise and no body to enter. */
    if (f->m->init_func == NULL) {
        *pm = f->m;
        return MODLINK_PROLOGUE;
    }
 pop:
    f = &w->frames[w->depth - 1];
    js_module_linking_scc_pop(ctx, f->m, &w->stack_top);      /* step 11 */
    w->depth--;
    w->resume = MLW_DESCEND;
#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
    if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) {
        printf("js_inner_module_linking done\n");
    }
#endif
    if (w->depth == 0) {
        w->resume = MLW_DONE;
        /* steps 5-6, and the ROOT is f->m because the last frame popped is the one init pushed */
        DCHECK(f->m->status == JS_MODULE_STATUS_LINKED ||
               f->m->status == JS_MODULE_STATUS_EVALUATING_ASYNC ||
               f->m->status == JS_MODULE_STATUS_EVALUATED,
               "16.2.1.6.1.2 step 5: Link returned with its own module not linked — step 11's SCC pop never "
               "reached the root, so its component is still LINKING and its bindings are half-built");
        DCHECK(w->stack_top == NULL,
               "16.2.1.6.1.2 step 6: Link finished with modules still on its stack — every one of them is "
               "left at LINKING for ever, and the next link of any graph that reaches one silently skips it");
        return MODLINK_DONE;
    }
    {
        JSModuleDef *child = f->m;
        JSModuleLinkFrame *pf = &w->frames[w->depth - 1];
        DCHECK(child->status == JS_MODULE_STATUS_LINKING || child->status == JS_MODULE_STATUS_LINKED ||
               child->status == JS_MODULE_STATUS_EVALUATING_ASYNC || child->status == JS_MODULE_STATUS_EVALUATED,
               "16.2.1.6.1.2.1 step 9.c.i: a child came back from its own InnerModuleLinking still UNLINKED");
        if (child->status == JS_MODULE_STATUS_LINKING)
            pf->m->dfs_ancestor_index = min_int(pf->m->dfs_ancestor_index, child->dfs_ancestor_index);
    }
    return MODLINK_MORE;
}

/* 16.2.1.6.1.2 Link ( ) step 4, run at the algorithm's own abrupt exit (step_module_link_run) and nowhere else.
   A flow dropped while parked inside a link needs no reset: every LINKING claim is a write to that flow's own
   delta (js_module_linking_enter announces it), so the claims die with the delta and a teardown writes into no
   world. `root` is the module Link was called on. It does not free `frames`, which `visit` declares. */
static void js_module_link_abrupt(JSContext *ctx, JSModuleLinkWalk *w, JSModuleDef *root)
{
    while (w->stack_top != NULL) {
        JSModuleDef *m1 = w->stack_top;
        DCHECK(m1->status == JS_MODULE_STATUS_LINKING,
               "16.2.1.6.1.2 step 4.a.i: a module on Link's own stack is not LINKING — step 8 appends only "
               "modules it has just set to linking, and this runs on the flow whose delta holds those writes");
        js_module_set_status(ctx, m1, JS_MODULE_STATUS_UNLINKED);
        w->stack_top = m1->stack_prev;
    }
    DCHECK(root->status == JS_MODULE_STATUS_UNLINKED,
           "16.2.1.6.1.2 step 4.b: Link threw and its own module is not unlinked — js_module_link_init claimed "
           "the root, and it leaves the stack only at the SCC pop that ends the walk");
    w->depth = 0;
    w->resume = MLW_DONE;
}

