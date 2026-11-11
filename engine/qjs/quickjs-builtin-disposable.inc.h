/* Explicit resource management */

enum {
    JS_DISPOSE_HINT_SYNC,   /* use: Call(method, value) */
    JS_DISPOSE_HINT_ADOPT,  /* adopt: Call(method, undefined, [value]) */
    JS_DISPOSE_HINT_DEFER,  /* defer: Call(method, undefined) */
};

typedef struct JSDisposableResource {
    JSValue value;
    JSValue method; /* dispose method */
    uint8_t hint;
} JSDisposableResource;

typedef struct JSDisposableStack {
    bool disposed;
    int resource_count;
    int resource_capacity;
    JSDisposableResource *resources;
} JSDisposableStack;

static JSValue js_new_suppressed_error(JSContext *ctx, JSValueConst error,
                                       JSValueConst suppressed, int backtrace_flags)
{
    JSValue obj;

    /* Construct via the intrinsic prototype rather than going through
       SuppressedError.prototype.constructor, which is user-writable. */
    obj = JS_NewObjectProtoClass(ctx,
                                 ctx->native_error_proto[JS_SUPPRESSED_ERROR],
                                 JS_CLASS_ERROR);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_error, js_dup(error),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_suppressed, js_dup(suppressed),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    /* No skip, for the reason the Error constructor has none: every caller of this reaches it from the
       interpreter or from a step machine, neither of which pushes a frame of its own. The one caller with a
       real builtin frame is the C-data closure below, and it says so at its call. */
    build_backtrace(ctx, obj, JS_UNDEFINED, NULL, 0, 0, backtrace_flags, NULL);
    return obj;
}

/* 27.3.3.4 DisposableStack.prototype.dispose ( ), i.e. DisposeResources with the sync-dispose hint.
   Every resource's dispose method is the PAGE'S CODE, so the loop that runs them is a step machine and each call
   is a request — the JS_Call loop that stood here entered a bytecode body by C recursion below the live flow, and
   a `finally` or a loop inside a dispose method could not park.
   It is also the first machine whose algorithm CATCHES a CALL's abrupt completion: step 3.d folds each throw into
   a SuppressedError and keeps disposing. That is what `catches_abrupt` declares, and the call-unwind arm that
   honours it for a CALL was built for this.
   THE RESOURCE LIST IS STOLEN, not iterated in place. A dispose method can call `move()` or `use()` on the very
   stack being disposed, so the array the old loop held a pointer into could be transferred or reallocated under
   it while user code ran — reachable from script, not a theoretical race. The machine owns the list from stage 0
   and its teardown releases whatever is left, so the stack object is inert for the whole disposal. */
/* fold this throw into the completion the spec carries: the first one IS the error, a later one SUPPRESSES it. */
static void js_dispose_fold_error(JSContext *ctx, JSDisposeRun *s)
{
    JSValue new_error = JS_GetException(ctx);
    if (JS_IsUninitialized(s->error)) {
        s->error = new_error;
        return;
    }
    {
        JSValue suppressed = js_new_suppressed_error(ctx, new_error, s->error, 0);
        JS_FreeValue(ctx, new_error);
        JS_FreeValue(ctx, s->error);
        /* SuppressedError's own construction can throw (an OOM, or a patched %SuppressedError%): that throw is
           then the completion, exactly as the value it failed to build would have been. */
        s->error = JS_IsException(suppressed) ? JS_GetException(ctx) : suppressed;
    }
}

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). The standard has no
   section number for any of the explicit-resource-management machines - it is not in the published edition, and
   every other number in this file is ES2025 - so each algorithm is NAMED and its steps are the proposal's own,
   exactly as the ArrayBuffer-base64 machines are (B64OP_STAGES). A number invented here would be a claim about
   the standard rather than a reference to it. */
#define DISPSYNC_STAGES(X) \
    X(DSY_ENTRY, "proposal-explicit-resource-management DisposableStack.prototype.dispose steps 1-4 (the " \
                 "receiver's [[DisposableState]]; an already-disposed stack returns undefined; the stack is " \
                 "marked disposed and its resource list is taken)") \
    X(DSY_CALL,  "proposal-explicit-resource-management DisposeResources step 3 (the LIFO walk: Call(method, " \
                 "value) for one resource; a throwing method is folded into a SuppressedError and the walk " \
                 "continues, which is why the throw is a value here)")
enum { DISPSYNC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_dispose_sync_steps[] = { DISPSYNC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_dispose_sync_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDisposeRun *s = st;

    if (s->hdr.stage == DSY_ENTRY) {
        JSDisposableStack *ds = JS_GetOpaque2(ctx, s->hdr.this_val, JS_CLASS_DISPOSABLE_STACK);
        JS_FreeValue(ctx, cb_result);
        s->error = JS_UNINITIALIZED;
        s->hdr.stage = DSY_CALL;
        if (!ds)
            return -1;
        if (ds->disposed)
            return 0;                       /* already disposed: undefined, and nothing to run */
        ds->disposed = true;
        s->res = ds->resources;             /* STEAL it — see above */
        s->n = ds->resource_count;
        ds->resources = NULL;
        ds->resource_count = 0;
        ds->resource_capacity = 0;
        s->i = s->n - 1;
    } else {
        /* the dispose method returned, or THREW and this machine catches it (catches_abrupt) */
        if (JS_IsException(cb_result))
            js_dispose_fold_error(ctx, s);
        else
            JS_FreeValue(ctx, cb_result);
        s->i--;
    }

    for (; s->i >= 0; s->i--) {
        JSDisposableResource *r = &s->res[s->i];
        JSValue method = r->method, value = r->value;
        r->method = JS_UNDEFINED;
        r->value = JS_UNDEFINED;            /* the machine hands both to the request; the list keeps neither */
        if (JS_IsUndefined(method)) {
            JS_FreeValue(ctx, value);       /* a null/undefined resource disposes to nothing */
            continue;
        }
        JS_FreeValue(ctx, s->cb[0]); JS_FreeValue(ctx, s->cb[1]); JS_FreeValue(ctx, s->cb[2]);
        s->cb[2] = JS_UNDEFINED;
        switch (r->hint) {
        case JS_DISPOSE_HINT_ADOPT:  s->cb[0] = JS_UNDEFINED; s->cb[2] = value;             break;
        case JS_DISPOSE_HINT_DEFER:  s->cb[0] = JS_UNDEFINED; JS_FreeValue(ctx, value);     break;
        default: /* SYNC */          s->cb[0] = value;                                      break;
        }
        s->cb[1] = method;
        *out_cb = s->cb;
        *out_argc = (r->hint == JS_DISPOSE_HINT_ADOPT) ? 1 : 0;
        return 3;                           /* CALL: the dispose method runs on the tramp and can park */
    }
    if (!JS_IsUninitialized(s->error)) {
        JS_Throw(ctx, s->error);
        s->error = JS_UNINITIALIZED;
        return -1;
    }
    return 0;
}

/* One STOLEN resource. Its own declaration because both disposal machines hold the same list and a sub-object
   array is visited element-wise; writing the two fields twice is the drift the declaration exists to stop. */
static void js_disposable_resource_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    JSDisposableResource *r = elem;
    v->val(ctx, &r->value);
    v->val(ctx, &r->method);
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The stolen list is owned only up to the LIFO cursor: the
   resources past it have already been disposed and released, so `i + 1` (clamped) is the live count in a list
   of `n`. The accumulated error is a JSValue whose UNINITIALIZED sentinel means "no error yet" — that tag
   carries no reference, so both consumers pass it through unchanged and the sentinel survives a fork.
   The stack object no longer holds the list, so on an ABANDON this is the only release there is. */
static void js_dispose_sync_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDisposeRun *s = st;
    int live = s->i + 1 < s->n ? s->i + 1 : s->n;
    v->array(ctx, (void **)&s->res, sizeof(JSDisposableResource), live < 0 ? 0 : live, s->n,
             js_disposable_resource_visit);
    v->val(ctx, &s->cb[0]); v->val(ctx, &s->cb[1]); v->val(ctx, &s->cb[2]);
    v->val(ctx, &s->error);
}


static const JSTrampStepDef js_dispose_sync_def = {
    sizeof(JSDisposeRun), js_dispose_sync_step, NULL, 0,
    /* body, body_proto, body_magic, precheck, midcheck, onerror */ {NULL}, 0, 0, NULL, NULL, NULL,
    .catches_abrupt = 1,  /* step 3.d: a throwing dispose method is a VALUE — it becomes the SuppressedError */
    .visit = js_dispose_sync_visit,
    .algorithm = "DisposableStack.prototype.dispose (proposal-explicit-resource-management)",
    .steps = js_dispose_sync_steps
};

static void js_disposable_stack_clear(JSRuntime *rt, JSDisposableStack *ds)
{
    int i;
    for (i = 0; i < ds->resource_count; i++) {
        JS_FreeValueRT(rt, ds->resources[i].value);
        JS_FreeValueRT(rt, ds->resources[i].method);
    }
    js_free_rt(rt, ds->resources);
}

static int js_disposable_stack_add(JSContext *ctx, JSDisposableStack *ds,
                                   JSValueConst value, JSValueConst method,
                                   int hint)
{
    if (ds->resource_count >= ds->resource_capacity) {
        int new_cap = max_int(ds->resource_capacity * 2, 4);
        JSDisposableResource *new_res;
        new_res = js_realloc(ctx, ds->resources,
                             new_cap * sizeof(JSDisposableResource));
        if (!new_res)
            return -1;
        ds->resources = new_res;
        ds->resource_capacity = new_cap;
    }
    ds->resources[ds->resource_count].value = js_dup(value);
    ds->resources[ds->resource_count].method = js_dup(method);
    ds->resources[ds->resource_count].hint = hint;
    ds->resource_count++;
    return 0;
}

/* ONE list expanded twice; not in the published edition, so named rather than numbered - see DISPSYNC_STAGES. */
#define SDWRAP_STAGES(X) \
    X(SDW_ENTRY, "proposal-explicit-resource-management GetDisposeMethod step 1.b.ii's closure, entered: " \
                 "the dispatch of Call(method, this)") \
    X(SDW_DONE,  "proposal-explicit-resource-management GetDisposeMethod step 1.b.ii's closure step 2 " \
                 "(the sync method's result is DISCARDED - an async disposal must not await it - and the " \
                 "closure returns undefined)")
enum { SDWRAP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_sync_dispose_wrap_steps[] = { SDWRAP_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_sync_dispose_wrap_step(JSContext *ctx, void *st, JSValue cb_result,
                                     JSValue **out_cb, int *out_argc)
{
    JSSyncDisposeWrap *s = st;
    if (s->hdr.stage == SDW_ENTRY) {
        JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
        JS_FreeValue(ctx, cb_result);
        s->hdr.stage = SDW_DONE;
        s->cb[0] = js_dup(s->hdr.this_val);
        s->cb[1] = js_dup(rec->data[0]);
        s->cb[2] = JS_UNDEFINED;
        *out_cb = s->cb; *out_argc = 0;
        return 3;
    }
    DCHECK(s->hdr.stage == SDW_DONE, "a sync-dispose wrapper resumed in an unknown stage");
    JS_FreeValue(ctx, cb_result);   /* DISCARDED: that is what the wrapper is for */
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Only the request buffer — the wrapper's value is always
   undefined, so there is no result a completion could hand out. */
static void js_sync_dispose_wrap_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSSyncDisposeWrap *s = st;
    v->val(ctx, &s->cb[0]); v->val(ctx, &s->cb[1]); v->val(ctx, &s->cb[2]);
}


static const JSTrampStepDef js_sync_dispose_wrap_def = {
    sizeof(JSSyncDisposeWrap), js_sync_dispose_wrap_step, NULL, 0, .visit = js_sync_dispose_wrap_visit,
    .algorithm = "GetDisposeMethod's sync-dispose wrapper (proposal-explicit-resource-management)",
    .steps = js_sync_dispose_wrap_steps
};

static JSValue js_new_sync_dispose_wrapper(JSContext *ctx, JSValue method)
{
    JSValueConst data[1];
    JSValue wrapped;
    data[0] = method;
    wrapped = js_new_step_closure(ctx, &js_sync_dispose_wrap_def, 0, 0, 1, data);
    JS_FreeValue(ctx, method);
    return wrapped;
}

/* 7.5.4 GetDisposeMethod ( value, kind ) + 7.5.3 CreateDisposableResource's object check, as THE step machine — one
   implementation for both callers. There were two: this C helper (for DisposableStack.prototype.use) and the
   `using` opcode's own inline reads, and both ran the [[Get]]s from C, so a dispose ACCESSOR or a Proxy get trap
   had no flow base. The reads are requests here, and `hint` rides the DEFINITION's `arg` — which is why there
   are two defs over one body, the same way Promise.resolve and Promise.reject are.
   The caller is responsible for the null/undefined case, because the two callers differ on it: `using` skips
   (no method), and an ASYNC DisposableStack still records the resource so disposeAsync performs its Await. */
typedef struct JSGetDisposeMethod {
    JSStepHdr hdr;      /* MUST be first */
    JSValue result;     /* owned: the method */
} JSGetDisposeMethod;
_Static_assert(offsetof(JSGetDisposeMethod, hdr) == 0, "JSStepHdr must be first in JSGetDisposeMethod");

/* ONE list expanded once PER HINT; not in the published edition, so named rather than numbered - see
   DISPSYNC_STAGES. The async hint reads @@asyncDispose FIRST and falls back to @@dispose, so it rests at two
   different steps of its own algorithm; the sync hint reads one property and its array is the shorter one. */
#define GDM_STAGES(X, FIRST, SYNC_GOT) \
    X(GDM_FIRST,     FIRST) \
    X(GDM_SYNC_GOT,  SYNC_GOT)
#define GDM_ASYNC_EXTRA(X, ASYNC_GOT) \
    X(GDM_ASYNC_GOT, ASYNC_GOT)
enum { GDM_STAGES(JS_STEP_STAGE_ENUM, 0, 0) GDM_ASYNC_EXTRA(JS_STEP_STAGE_ENUM, 0) };
static const char *const js_get_dispose_sync_steps[] = {
    GDM_STAGES(JS_STEP_STAGE_LABEL,
        "proposal-explicit-resource-management GetDisposeMethod with hint sync-dispose, entered: V is an Object, "
        "and the dispatch of step 2's GetMethod(V, %Symbol.dispose%)",
        "proposal-explicit-resource-management GetDisposeMethod with hint sync-dispose step 2 (method arrives "
        "and must be callable)")
    NULL };
static const char *const js_get_dispose_async_steps[] = {
    GDM_STAGES(JS_STEP_STAGE_LABEL,
        "proposal-explicit-resource-management GetDisposeMethod with hint async-dispose, entered: V is an "
        "Object, and the dispatch of step 1.a's GetMethod(V, %Symbol.asyncDispose%)",
        "proposal-explicit-resource-management GetDisposeMethod step 1.b (the @@dispose method arrives and is "
        "WRAPPED, because an async disposal must not await what a sync method returns)")
    GDM_ASYNC_EXTRA(JS_STEP_STAGE_LABEL,
        "proposal-explicit-resource-management GetDisposeMethod step 1.a (the @@asyncDispose method arrives; "
        "undefined or null falls through to step 1.b's @@dispose read)")
    NULL };

static int js_get_dispose_method_step(JSContext *ctx, void *st, JSValue cb_result,
                                      JSValue **out_cb, int *out_argc)
{
    JSGetDisposeMethod *s = st;
    bool is_async = (s->hdr.arg != 0);
    JSValue method;

    if (s->hdr.stage == GDM_FIRST) {
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        if (!JS_IsObject(step_arg(&s->hdr, 0))) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        s->hdr.stage = is_async ? GDM_ASYNC_GOT : GDM_SYNC_GOT;
        s->result = js_dup(step_arg(&s->hdr, 0));   /* the request BORROWS cb[0]; the machine holds it */
        *out_cb = &s->result;
        *out_argc = (int)(is_async ? JS_ATOM_Symbol_asyncDispose : JS_ATOM_Symbol_dispose);
        return 6;   /* GETPROP */
    }
    method = cb_result;
    JS_FreeValue(ctx, s->result); s->result = JS_UNDEFINED;
    if (s->hdr.stage == GDM_ASYNC_GOT && (JS_IsUndefined(method) || JS_IsNull(method))) {
        /* step 1.b: no @@asyncDispose, so @@dispose is read instead — and its method is WRAPPED, because an
           async disposal must not await what a sync method returns. */
        JS_FreeValue(ctx, method);
        s->hdr.stage = GDM_SYNC_GOT;
        s->result = js_dup(step_arg(&s->hdr, 0));
        *out_cb = &s->result; *out_argc = (int)JS_ATOM_Symbol_dispose;
        return 6;
    }
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        JS_ThrowTypeError(ctx, "value is not disposable");
        return -1;
    }
    if (!JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        JS_ThrowTypeError(ctx, "dispose method is not a function");
        return -1;
    }
    if (is_async && s->hdr.stage == GDM_SYNC_GOT) {
        method = js_new_sync_dispose_wrapper(ctx, method);   /* consumes it */
        if (unlikely(JS_IsException(method))) return -1;
    }
    s->result = method;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The method it read. */
static void js_get_dispose_method_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSGetDisposeMethod *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_get_dispose_method_fini(JSContext *ctx, void *st, bool take_result)
{
    JSGetDisposeMethod *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_get_dispose_sync_def = {
    sizeof(JSGetDisposeMethod), js_get_dispose_method_step, js_get_dispose_method_fini, 0, .visit = js_get_dispose_method_visit,
    .algorithm = "GetDisposeMethod, hint sync-dispose (proposal-explicit-resource-management)",
    .steps = js_get_dispose_sync_steps
};
static const JSTrampStepDef js_get_dispose_async_def = {
    sizeof(JSGetDisposeMethod), js_get_dispose_method_step, js_get_dispose_method_fini, 1, .visit = js_get_dispose_method_visit,
    .algorithm = "GetDisposeMethod, hint async-dispose (proposal-explicit-resource-management)",
    .steps = js_get_dispose_async_steps
};

static int js_disposable_ctor_precheck(JSContext *ctx, const JSStepHdr *h)
{
    /* 27.3.1.1 DisposableStack ( ) step 1 / 27.4.1.1 AsyncDisposableStack ( ) step 1: NewTarget undefined means the CALL form, which these constructors
       reject before anything is created — and therefore before the `prototype` read. */
    if (JS_IsUndefined(h->this_val)) {
        JS_ThrowTypeError(ctx, "Constructor requires 'new'");
        return -1;
    }
    return 0;
}

/* Steps 3-4: the created object gets its [[DisposeCapability]]. The class id is already baked into `obj`.
   The body CONSUMES the object js_creatector_step created — its return value IS the machine's result, so a
   js_dup here leaks the step's own reference on every construction (the gc_obj_list walk caught exactly that). */
static JSValue js_disposable_ctor_body(JSContext *ctx, JSValueConst obj_, int argc, JSValueConst *argv)
{
    JSValue obj = (JSValue)obj_;
    JSDisposableStack *s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_SetOpaqueInternal(obj, s);
    return obj;
}

static void js_disposable_stack_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSObject *p;
    JSDisposableStack *s;

    p = JS_VALUE_GET_OBJ(val);
    s = p->u.opaque;
    if (s) {
        js_disposable_stack_clear(rt, s);
        js_free_rt(rt, s);
    }
}

static void js_disposable_stack_mark(JSRuntime *rt, JSValueConst val,
                                     JS_MarkFunc *mark_func)
{
    JSObject *p;
    JSDisposableStack *s;
    int i;

    p = JS_VALUE_GET_OBJ(val);
    s = p->u.opaque;
    if (s) {
        for (i = 0; i < s->resource_count; i++) {
            JS_MarkValue(rt, s->resources[i].value, mark_func);
            JS_MarkValue(rt, s->resources[i].method, mark_func);
        }
    }
}

static JSDisposableStack *js_disposable_stack_get(JSContext *ctx,
                                                  JSValueConst this_val,
                                                  int class_id)
{
    JSDisposableStack *s;
    s = JS_GetOpaque2(ctx, this_val, class_id);
    if (!s)
        return NULL;
    if (s->disposed) {
        JS_ThrowReferenceError(ctx, "DisposableStack has been disposed");
        return NULL;
    }
    return s;
}

/* 27.3.3.7 DisposableStack.prototype.use ( value ) / 27.4.3.7 AsyncDisposableStack.prototype.use as a STEP MACHINE. Its one page-visible step is
   GetDisposeMethod, which it DELEGATES to — the same machine the `using` opcode drives, so there is one
   implementation of 7.5.4 GetDisposeMethod and not a builtin's copy beside an opcode's. `arg` carries the class id, exactly
   as the C body's magic did. */
typedef struct JSDisposableUse {
    JSStepHdr hdr;      /* MUST be first */
    JSValue result;     /* owned: the value, which `use` returns */
} JSDisposableUse;
_Static_assert(offsetof(JSDisposableUse, hdr) == 0, "JSStepHdr must be first in JSDisposableUse");

/* ONE list expanded once per stack class; not in the published edition, so named rather than numbered - see
   DISPSYNC_STAGES. */
#define DUSE_STAGES(X, ENTRY, GOT) \
    X(DUSE_ENTRY, ENTRY) \
    X(DUSE_GOT,   GOT)
enum { DUSE_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_disposable_use_steps[] = {
    DUSE_STAGES(JS_STEP_STAGE_LABEL,
        "proposal-explicit-resource-management DisposableStack.prototype.use steps 1-4 (the receiver's "
        "[[DisposableState]] must be pending; a null or undefined value is returned unchanged) - and the "
        "delegation of step 5's AddDisposableResource, whose GetDisposeMethod is a machine of its own",
        "proposal-explicit-resource-management DisposableStack.prototype.use step 5 (the dispose method "
        "arrives; the resource is recorded against the stack the receiver holds NOW, because the getter or "
        "trap that produced the method is the page's code) - and step 6, Return value")
    NULL };
static const char *const js_async_disposable_use_steps[] = {
    DUSE_STAGES(JS_STEP_STAGE_LABEL,
        "proposal-explicit-resource-management AsyncDisposableStack.prototype.use steps 1-4 (the receiver's "
        "[[AsyncDisposableState]] must be pending; a null or undefined value is still RECORDED, so "
        "disposeAsync performs the Await it owes) - and the delegation of step 5's AddDisposableResource",
        "proposal-explicit-resource-management AsyncDisposableStack.prototype.use step 5 (the dispose method "
        "arrives and the resource is recorded) - and step 6, Return value")
    NULL };

static int js_disposable_stack_use_step(JSContext *ctx, void *st, JSValue cb_result,
                                        JSValue **out_cb, int *out_argc)
{
    JSDisposableUse *s = st;
    int class_id = s->hdr.arg;
    JSValueConst value = step_arg(&s->hdr, 0);
    JSDisposableStack *ds;

    ds = js_disposable_stack_get(ctx, s->hdr.this_val, class_id);
    if (!ds) { if (s->hdr.stage == DUSE_ENTRY) s->result = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return -1; }

    if (s->hdr.stage == DUSE_ENTRY) {
        void *inner;
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        if (JS_IsNull(value) || JS_IsUndefined(value)) {
            /* an ASYNC stack still records it, so disposeAsync performs the required Await(undefined). */
            if (class_id == JS_CLASS_ASYNC_DISPOSABLE_STACK
                && js_disposable_stack_add(ctx, ds, JS_UNDEFINED, JS_UNDEFINED, JS_DISPOSE_HINT_SYNC) < 0)
                return -1;
            s->result = js_dup(value);
            return 0;
        }
        s->hdr.stage = DUSE_GOT;
        inner = tramp_step_state_new(ctx,
                                     class_id == JS_CLASS_ASYNC_DISPOSABLE_STACK ? &js_get_dispose_async_def
                                                                                 : &js_get_dispose_sync_def,
                                     JS_UNDEFINED, 1, &value, JS_UNDEFINED);
        if (!inner) return -1;
        s->hdr.delegate = inner;
        return 17;   /* DELEGATE */
    }
    DCHECK(s->hdr.stage == DUSE_GOT, "DisposableStack.prototype.use resumed in an unknown stage");
    /* cb_result = the dispose method. The RECORD is added under the stack the receiver holds NOW: the getter or
       trap that produced the method is the page's code and can have disposed or replaced it, which is why the
       receiver is re-validated above rather than captured at stage 0. */
    if (js_disposable_stack_add(ctx, ds, value, cb_result, JS_DISPOSE_HINT_SYNC) < 0) {
        JS_FreeValue(ctx, cb_result);
        return -1;
    }
    JS_FreeValue(ctx, cb_result);
    s->result = js_dup(value);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The value `use` returns. */
static void js_disposable_stack_use_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDisposableUse *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_disposable_stack_use_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDisposableUse *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_disposable_use_def = {
    sizeof(JSDisposableUse), js_disposable_stack_use_step, js_disposable_stack_use_fini,
    JS_CLASS_DISPOSABLE_STACK, .visit = js_disposable_stack_use_visit,
    .algorithm = "DisposableStack.prototype.use (proposal-explicit-resource-management)",
    .steps = js_disposable_use_steps
};
static const JSTrampStepDef js_async_disposable_use_def = {
    sizeof(JSDisposableUse), js_disposable_stack_use_step, js_disposable_stack_use_fini,
    JS_CLASS_ASYNC_DISPOSABLE_STACK, .visit = js_disposable_stack_use_visit,
    .algorithm = "AsyncDisposableStack.prototype.use (proposal-explicit-resource-management)",
    .steps = js_async_disposable_use_steps
};

static JSValue js_disposable_stack_adopt(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int class_id)
{
    JSDisposableStack *s;
    JSValueConst value, on_dispose;

    s = js_disposable_stack_get(ctx, this_val, class_id);
    if (!s)
        return JS_EXCEPTION;
    value = argv[0];
    on_dispose = argv[1];
    if (!JS_IsFunction(ctx, on_dispose))
        return JS_ThrowTypeError(ctx, "not a function");
    if (js_disposable_stack_add(ctx, s, value, on_dispose, JS_DISPOSE_HINT_ADOPT) < 0)
        return JS_EXCEPTION;
    return js_dup(value);
}

static JSValue js_disposable_stack_defer(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int class_id)
{
    JSDisposableStack *s;
    JSValueConst on_dispose;

    s = js_disposable_stack_get(ctx, this_val, class_id);
    if (!s)
        return JS_EXCEPTION;
    on_dispose = argv[0];
    if (!JS_IsFunction(ctx, on_dispose))
        return JS_ThrowTypeError(ctx, "not a function");
    if (js_disposable_stack_add(ctx, s, JS_UNDEFINED, on_dispose, JS_DISPOSE_HINT_DEFER) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* Simple .then handler that discards its argument and returns undefined;
   used to normalize the chain's resolved value after all disposals. */
static JSValue js_async_dispose_to_undef(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv,
                                         int magic, JSValueConst *func_data)
{
    return JS_UNDEFINED;
}

static JSValue js_async_dispose_rethrow(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv,
                                        int magic, JSValueConst *func_data)
{
    JSValue prev_err = js_dup(func_data[0]);
    if (magic == 0) {
        return JS_Throw(ctx, prev_err);
    } else {
        /* THE SLOT THIS READS EXISTS BECAUSE THE CLOSURE DECLARED IT. js_call_c_function_data pads arg_buf out
           to the record's `length` and no further, so a body that reads argv[0] while declaring arity 0 is
           reading the CALLER's stack the moment a zero-argument call arrives — and the value it then hands to
           js_new_suppressed_error is whatever that slot held, freed or not. The arity declaration is the whole
           of the guarantee, which is why it is asserted here rather than trusted: the reachability that makes
           a misdeclaration harmless (this chain is built on PerformPromiseThen, so no page code ever holds this
           handler to call it with no arguments) is a property of the CALLERS, and callers are added. */
        DCHECK(argc >= 1, "an async-dispose rethrow handler was called with no arguments — its closure declares "
                          "arity 1 so js_call_c_function_data pads the slot; this one did not");
        /* THIS one has a frame of its own: the rejection handler is a C closure, and js_call_c_function_data
           pushed a JSStackFrame for it. It is engine plumbing, not a place in the page, so it is skipped. */
        JSValue se = js_new_suppressed_error(ctx, argv[0], prev_err, JS_BACKTRACE_FLAG_SKIP_FIRST_LEVEL);
        JS_FreeValue(ctx, prev_err);
        if (JS_IsException(se))
            return JS_EXCEPTION;
        return JS_Throw(ctx, se);
    }
}

/* ONE link of disposeAsync's chain: dispose this resource, then Await its result. `func_data` is
   [value, method, hint] and the magic says whether a previous link already failed (so this one's completion is a
   SuppressedError). The dispose method is the page's code, so it is a CALL request rather than a JS_Call — the
   closure declares itself a step machine and the job pump's reaction dispatch drives it on the tramp.
   AND SO IS THE AWAIT THAT FOLLOWS IT, which is the half this machine used to perform from C: the value the
   method returned is the page's OBJECT, and 27.5.4.7.1 over it reads `constructor` and then `then`. It runs as
   step_promiseresolve_run on this same flow. */
/* ONE list expanded twice; not in the published edition, so named rather than numbered - see DISPSYNC_STAGES. */
#define ADLINK_STAGES(X) \
    X(ADL_ENTRY, "proposal-explicit-resource-management DisposeResources' async chain, one link entered: " \
                 "the dispatch of Call(method, value) for this resource, or Await(undefined) when it has none") \
    X(ADL_DONE,  "proposal-explicit-resource-management DisposeResources' async chain, one link's method " \
                 "settled: its value becomes the next link's Await, and a throw becomes the completion - " \
                 "wrapped in a SuppressedError when this link already carries one") \
    X(ADL_AWAIT, "proposal-explicit-resource-management DisposeResources' async chain, one link's Await -> " \
                 "27.5.4.7.1 PromiseResolve ( ctor, resolution ) steps 1-4 (this link CARRIES an error, so " \
                 "the method's value is awaited before it is re-raised; step 1.a's Get(resolution, " \
                 "\"constructor\") and the resolving function's Get(resolution, \"then\") are the page's code)")
enum { ADLINK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_async_dispose_link_steps[] = { ADLINK_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_async_dispose_link_step(JSContext *ctx, void *st, JSValue cb_result,
                                      JSValue **out_cb, int *out_argc)
{
    JSAsyncDisposeLink *s = st;
    JSCFunctionDataRecord *rec = JS_VALUE_GET_OBJ(s->hdr.func_obj)->u.c_function_data_record;
    bool has_prev_err = (rec->magic == 1);

    if (s->hdr.stage == ADL_ENTRY) {
        JSValueConst method = rec->data[1];
        int hint = JS_VALUE_GET_INT(rec->data[2]);
        JS_FreeValue(ctx, cb_result);
        s->hdr.stage = ADL_DONE;
        /* A COMPLETION IS WRITTEN, NEVER LEFT. tramp_step_state_new zeroes the state block and a zeroed JSValue
           is the INTEGER 0 — an ordinary value, indistinguishable at the teardown from a completion this link
           computed — so the slot carries the same "no operand yet" sentinel the header uses for its own, and
           the fini asserts every normal return wrote over it. */
        s->result = JS_UNINITIALIZED;
        /* EVERY OWNED SLOT IS PLACED BEFORE THE FIRST OPERATION THAT CAN THROW, because the failure path tears
           this state down through the visit, which frees exactly what the state holds. */
        s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
        s->cb[2] = JS_UNDEFINED; s->cb[3] = JS_UNDEFINED;
        s->pr_x = JS_UNDEFINED;
        if (JS_IsUndefined(method)) {
            /* a null/undefined resource on an async stack still performs Await(undefined) */
            if (!has_prev_err) {
                s->result = JS_UNDEFINED;   /* Await(undefined): the value the next link awaits */
                return 0;
            }
            JS_Throw(ctx, js_dup(step_arg(&s->hdr, 0)));
            return -1;
        }
        s->cb[1] = js_dup(method);
        switch (hint) {
        case JS_DISPOSE_HINT_ADOPT: s->cb[2] = js_dup(rec->data[0]); *out_argc = 1; break;
        case JS_DISPOSE_HINT_DEFER:                                  *out_argc = 0; break;
        default: /* SYNC */         s->cb[0] = js_dup(rec->data[0]); *out_argc = 0; break;
        }
        *out_cb = s->cb;
        return 3;
    }
    if (s->hdr.stage == ADL_DONE) {
        /* the dispose call is over, so its operands are dead — and the PromiseResolve below reuses this buffer,
           so they are released HERE rather than left for the teardown. */
        JS_FreeValue(ctx, s->cb[0]); s->cb[0] = JS_UNDEFINED;
        JS_FreeValue(ctx, s->cb[1]); s->cb[1] = JS_UNDEFINED;
        JS_FreeValue(ctx, s->cb[2]); s->cb[2] = JS_UNDEFINED;
        if (JS_IsException(cb_result)) {
            JSValue new_err = JS_GetException(ctx);
            if (has_prev_err) {
                JSValue se = js_new_suppressed_error(ctx, new_err, step_arg(&s->hdr, 0), 0);
                JS_FreeValue(ctx, new_err);
                if (JS_IsException(se))
                    return -1;
                JS_Throw(ctx, se);
                return -1;
            }
            JS_Throw(ctx, new_err);
            return -1;
        }
        if (!has_prev_err) {
            s->result = cb_result;   /* the next link awaits it */
            return 0;
        }
        /* Await the method's result, then re-raise the error this link is carrying (wrapped, if the method's own
           completion suppresses it). The Await's PromiseResolve reads `constructor` off a promise and `then` off
           a thenable — both the page's code — so it is a SUB-SEQUENCE on this flow, and the stage moves here
           because a keyed request may not span a stage boundary (see step_keyed_inflight). */
        s->pr_x = cb_result;
        cb_result = JS_UNDEFINED;
        s->pr_phase = PRR_START;
        s->hdr.stage = ADL_AWAIT;
    }
    DCHECK(s->hdr.stage == ADL_AWAIT, "an async dispose link resumed in an unknown stage");
    {
        JSValueConst prev_err = step_arg(&s->hdr, 0);
        JSValue ret_promise, resolve_fn, reject_fn, then_args[2];
        int r = step_promiseresolve_run(ctx, &s->hdr, &s->pr_phase, s->pr_x, &ret_promise, s->cb,
                                        cb_result, out_cb, out_argc);
        if (r) return r;
        JS_FreeValue(ctx, s->pr_x); s->pr_x = JS_UNDEFINED;
        /* THE ARITY IS THE READ, DECLARED. The fulfilment spelling reads no operand and declares none; the
           rejection spelling reads argv[0] and therefore declares 1, which is what makes js_call_c_function_data
           pad the slot for a call that supplies nothing. */
        resolve_fn = JS_NewCFunctionData(ctx, js_async_dispose_rethrow, 0, 0, 1, &prev_err);
        reject_fn  = JS_NewCFunctionData(ctx, js_async_dispose_rethrow, 1, 1, 1, &prev_err);
        if (JS_IsException(resolve_fn) || JS_IsException(reject_fn)) {
            JS_FreeValue(ctx, resolve_fn); JS_FreeValue(ctx, reject_fn);
            JS_FreeValue(ctx, ret_promise);
            return -1;
        }
        then_args[0] = resolve_fn;
        then_args[1] = reject_fn;
        s->result = js_promise_then_native(ctx, ret_promise, vc(then_args));
        JS_FreeValue(ctx, resolve_fn);
        JS_FreeValue(ctx, reject_fn);
        JS_FreeValue(ctx, ret_promise);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The link's promise, its request buffer — which the dispose
   call and the Await's PromiseResolve share — and the resolution that sub-run borrows across a park. */
static void js_async_dispose_link_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAsyncDisposeLink *s = st;
    int i;
    v->val(ctx, &s->result);
    for (i = 0; i < 4; i++) v->val(ctx, &s->cb[i]);
    v->val(ctx, &s->pr_x);
}

static JSValue js_async_dispose_link_fini(JSContext *ctx, void *st, bool take_result)
{
    JSAsyncDisposeLink *s = st;
    JSValue r;
    DCHECK(!take_result || !JS_IsUninitialized(s->result),
           "an async dispose link completed without writing its completion — the teardown would hand out the "
           "zeroed state block's INTEGER 0 as the value the next link awaits");
    r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_async_dispose_link_def = {
    sizeof(JSAsyncDisposeLink), js_async_dispose_link_step, js_async_dispose_link_fini, 0,
    /* body, body_proto, body_magic, precheck, midcheck, onerror */ {NULL}, 0, 0, NULL, NULL, NULL,
    .catches_abrupt = 1,  /* a throwing dispose method is this algorithm's VALUE: it becomes the completion */
    .visit = js_async_dispose_link_visit,
    .algorithm = "DisposeResources' async chain link (proposal-explicit-resource-management)",
    .steps = js_async_dispose_link_steps
};

/* Build one chain link's two reaction closures over [value, method, hint], both declared step machines. The
   resource's value and method are CONSUMED. */
static int js_async_dispose_link_new(JSContext *ctx, JSValue value, JSValue method, int hint,
                                     JSValue *out_resolve, JSValue *out_reject)
{
    JSValueConst data[3];
    JSValue hint_val = JS_NewInt32(ctx, hint);
    data[0] = value; data[1] = method; data[2] = hint_val;
    /* The arity states what the spelling READS, the same as the rethrow closure's: the fulfilment link consumes
       nothing the Await delivered, the rejection link carries the previous error in operand 0. It is not what
       makes the read in-bounds here — a step machine takes the CALL's real operands and step_arg answers past
       them with undefined — but a declared arity that disagrees with the body is the drift that sends the next
       reader to the safe half of the pair. */
    *out_resolve = js_new_step_closure(ctx, &js_async_dispose_link_def, 0, 0, 3, data);
    *out_reject  = js_new_step_closure(ctx, &js_async_dispose_link_def, 1, 1, 3, data);
    JS_FreeValue(ctx, hint_val);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, method);
    if (JS_IsException(*out_resolve) || JS_IsException(*out_reject)) {
        JS_FreeValue(ctx, *out_resolve); JS_FreeValue(ctx, *out_reject);
        *out_resolve = JS_UNDEFINED; *out_reject = JS_UNDEFINED;
        return -1;
    }
    return 0;
}

/* 27.4.3.4 AsyncDisposableStack.prototype.disposeAsync ( ) -> DisposeResources with the async-dispose hint.
   The FIRST resource's dispose method is called SYNCHRONOUSLY (the Await comes after it), so that one call is a
   request made by this machine; the rest become a chain of js_async_dispose_link closures, each of which is a
   step machine too. The resource list is STOLEN for the reason the sync machine steals it — that first call is
   the page's code and can reach move()/use() on this same stack. */
static void js_dispose_async_drop(JSContext *ctx, JSDisposeAsync *s, int from)
{
    int i;
    for (i = 0; i <= from && i < s->n; i++) {
        JS_FreeValue(ctx, s->res[i].value);
        JS_FreeValue(ctx, s->res[i].method);
        s->res[i].value = JS_UNDEFINED;
        s->res[i].method = JS_UNDEFINED;
    }
}

/* The settles that reach NO page code: a rejection (27.5.1.3 step 4.d is RejectPromise and nothing else) and a
   resolution with undefined (step 2.e is FulfillPromise). The METHOD's value is not one of them and does not
   come through here — it runs 27.5.4.7.1 as a sub-sequence on the flow; js_promise_resolve_native asserts the
   difference at its own door rather than trusting this call site to remember it. */
static JSValue js_dispose_async_settled(JSContext *ctx, JSValue v, int is_reject)
{
    JSValue p = js_promise_resolve_native(ctx, ctx->promise_ctor, 1, vc(&v), is_reject);
    JS_FreeValue(ctx, v);
    return p;
}

/* ONE list expanded twice; not in the published edition, so named rather than numbered - see DISPSYNC_STAGES. */
#define DASYNC_STAGES(X) \
    X(DAS_ENTRY, "proposal-explicit-resource-management AsyncDisposableStack.prototype.disposeAsync steps 1-5 " \
                 "(every failure here is a REJECTION, never a throw; an already-disposed stack settles with " \
                 "undefined; the stack is marked disposed and its resource list is taken)") \
    X(DAS_CHAIN, "proposal-explicit-resource-management AsyncDisposableStack.prototype.disposeAsync, the chain " \
                 "built: the remaining resources are linked LIFO, each link Awaiting the one above it") \
    X(DAS_FIRST, "proposal-explicit-resource-management DisposeResources step 3, the TOP resource: " \
                 "Call(method, value) - its settlement is what the rest of the chain is attached to") \
    X(DAS_RESOLVE, "proposal-explicit-resource-management DisposeResources step 3, the TOP resource's VALUE -> " \
                   "27.5.4.7.1 PromiseResolve ( ctor, resolution ) steps 1-4 (the settled promise the chain " \
                   "hangs off; step 1.a's Get(resolution, \"constructor\") and the resolving function's " \
                   "Get(resolution, \"then\") are the page's code)")
enum { DASYNC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_dispose_async_steps[] = { DASYNC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_dispose_async_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDisposeAsync *s = st;

    if (s->hdr.stage == DAS_ENTRY) {
        JSDisposableStack *ds = JS_GetOpaque2(ctx, s->hdr.this_val, JS_CLASS_ASYNC_DISPOSABLE_STACK);
        JSDisposableResource *top;
        JS_FreeValue(ctx, cb_result);
        s->hdr.stage = DAS_CHAIN;
        /* EVERY OWNED SLOT IS PLACED BEFORE THE FIRST OPERATION THAT CAN THROW — the settle below allocates,
           and its failure path tears this state down through the visit, which frees what the state holds. */
        s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
        s->cb[2] = JS_UNDEFINED; s->cb[3] = JS_UNDEFINED;
        s->pr_x = JS_UNDEFINED;
        if (!ds) {
            /* every failure of this builtin is a REJECTION, never a throw */
            s->result = js_dispose_async_settled(ctx, JS_GetException(ctx), 1);
            return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
        }
        if (ds->disposed) {
            s->result = js_dispose_async_settled(ctx, JS_UNDEFINED, 0);
            return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
        }
        ds->disposed = true;
        s->res = ds->resources;
        s->n = ds->resource_count;
        ds->resources = NULL;
        ds->resource_count = 0;
        ds->resource_capacity = 0;
        s->i = s->n - 1;
        if (s->i < 0) {
            s->result = js_dispose_async_settled(ctx, JS_UNDEFINED, 0);
            return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
        }
        top = &s->res[s->i];
        if (!JS_IsUndefined(top->method)) {
            JSValue method = top->method, value = top->value;
            top->method = JS_UNDEFINED;
            top->value = JS_UNDEFINED;
            s->cb[1] = method;
            switch (top->hint) {
            case JS_DISPOSE_HINT_ADOPT: s->cb[2] = value;                        *out_argc = 1; break;
            case JS_DISPOSE_HINT_DEFER: JS_FreeValue(ctx, value);                *out_argc = 0; break;
            default: /* SYNC */         s->cb[0] = value;                        *out_argc = 0; break;
            }
            *out_cb = s->cb;
            s->hdr.stage = DAS_FIRST;
            return 3;
        }
        /* a null/undefined top resource: its Await(undefined) IS the start of the chain */
        JS_FreeValue(ctx, top->value);
        top->value = JS_UNDEFINED;
        cb_result = JS_UNDEFINED;
        s->result = js_dispose_async_settled(ctx, JS_UNDEFINED, 0);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; js_dispose_async_drop(ctx, s, s->i - 1); return -1; }
    } else if (s->hdr.stage == DAS_FIRST) {
        /* the dispose call is over, so its operands are dead — and the PromiseResolve below reuses this buffer,
           so they are released HERE rather than left for the teardown. */
        JS_FreeValue(ctx, s->cb[0]); s->cb[0] = JS_UNDEFINED;
        JS_FreeValue(ctx, s->cb[1]); s->cb[1] = JS_UNDEFINED;
        JS_FreeValue(ctx, s->cb[2]); s->cb[2] = JS_UNDEFINED;
        /* the first dispose method returned or threw; either way it starts the chain as a SETTLED promise */
        if (JS_IsException(cb_result)) {
            /* a REJECTION reaches no page code at all — 27.5.1.3 step 4.d is RejectPromise and nothing else —
               so it stays the C algorithm the settle helper already is. */
            s->result = js_dispose_async_settled(ctx, JS_GetException(ctx), 1);
            if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; js_dispose_async_drop(ctx, s, s->i - 1); return -1; }
            s->hdr.stage = DAS_CHAIN;
        } else {
            /* THE METHOD'S VALUE IS THE PAGE'S OBJECT. PromiseResolve over it reads `constructor` (step 1.a)
               and, through the capability's resolving function, `then` (27.5.1.3 step 2.f) — both an accessor
               or a Proxy trap away from being the page's code, and a C activation has no flow base for them. So
               it is a SUB-SEQUENCE on this flow, and the stage moves because a keyed request may not span a
               stage boundary (see step_keyed_inflight). */
            s->pr_x = cb_result;
            cb_result = JS_UNDEFINED;
            s->result = JS_UNDEFINED;
            s->pr_phase = PRR_START;
            s->hdr.stage = DAS_RESOLVE;
        }
    }
    if (s->hdr.stage == DAS_RESOLVE) {
        int r = step_promiseresolve_run(ctx, &s->hdr, &s->pr_phase, s->pr_x, &s->result, s->cb,
                                        cb_result, out_cb, out_argc);
        if (r) {
            if (r < 0) js_dispose_async_drop(ctx, s, s->i - 1);
            return r;
        }
        JS_FreeValue(ctx, s->pr_x); s->pr_x = JS_UNDEFINED;
        s->hdr.stage = DAS_CHAIN;
    }
    DCHECK(s->hdr.stage == DAS_CHAIN, "disposeAsync reached its chain build in an unknown stage");
    s->i--;

    for (; s->i >= 0; s->i--) {
        JSDisposableResource *r = &s->res[s->i];
        JSValue value = r->value, method = r->method, resolve_fn, reject_fn, then_args[2], new_chain;
        r->value = JS_UNDEFINED;
        r->method = JS_UNDEFINED;
        if (js_async_dispose_link_new(ctx, value, method, r->hint, &resolve_fn, &reject_fn) < 0) {
            js_dispose_async_drop(ctx, s, s->i - 1);
            return -1;
        }
        then_args[0] = resolve_fn;
        then_args[1] = reject_fn;
        /* DisposeResources' async chain is a sequence of AWAITs, and Await is PerformPromiseThen on a
           capability — never Promise.prototype.then, which would READ a property and CALL a method. Every link
           of `chain` is an engine-built native promise, so the abstract operation is what this always was. */
        new_chain = js_promise_then_native(ctx, s->result, vc(then_args));
        JS_FreeValue(ctx, resolve_fn);
        JS_FreeValue(ctx, reject_fn);
        JS_FreeValue(ctx, s->result);
        s->result = JS_UNDEFINED;
        if (JS_IsException(new_chain)) { js_dispose_async_drop(ctx, s, s->i - 1); return -1; }
        s->result = new_chain;
    }
    {
        /* the same Await with only an onFulfilled: PerformPromiseThen takes both handlers, and an absent
           onRejected IS undefined. It normalises the chain's value to undefined. */
        JSValue undef_fn = JS_NewCFunctionData(ctx, js_async_dispose_to_undef, 0, 0, 0, NULL);
        JSValue then_args[2], new_chain;
        if (JS_IsException(undef_fn))
            return -1;
        then_args[0] = undef_fn;
        then_args[1] = JS_UNDEFINED;
        new_chain = js_promise_then_native(ctx, s->result, vc(then_args));
        JS_FreeValue(ctx, undef_fn);
        JS_FreeValue(ctx, s->result);
        s->result = JS_IsException(new_chain) ? JS_UNDEFINED : new_chain;
        return JS_IsException(new_chain) ? -1 : 0;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The same partial ownership of the stolen list the sync
   machine has — everything up to the cursor, which is the un-chained tail an abandon leaves owned here and
   nowhere else. The chaining steps release ahead of the cursor by writing UNDEFINED into the slot, so a slot
   inside the live range that has already gone is a value that holds nothing, not a second free. */
static void js_dispose_async_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDisposeAsync *s = st;
    int live = s->i + 1 < s->n ? s->i + 1 : s->n;
    int i;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->res, sizeof(JSDisposableResource), live < 0 ? 0 : live, s->n,
             js_disposable_resource_visit);
    for (i = 0; i < 4; i++) v->val(ctx, &s->cb[i]);
    v->val(ctx, &s->pr_x);
}

static JSValue js_dispose_async_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDisposeAsync *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_dispose_async_def = {
    sizeof(JSDisposeAsync), js_dispose_async_step, js_dispose_async_fini, 0,
    /* body, body_proto, body_magic, precheck, midcheck, onerror */ {NULL}, 0, 0, NULL, NULL, NULL,
    .catches_abrupt = 1,  /* the first dispose method's throw REJECTS the returned promise, it does not propagate */
    .visit = js_dispose_async_visit,
    .algorithm = "AsyncDisposableStack.prototype.disposeAsync (proposal-explicit-resource-management)",
    .steps = js_dispose_async_steps
};

static JSValue js_disposable_stack_move(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int class_id)
{
    JSDisposableStack *s, *ns;
    JSValue new_obj;

    s = js_disposable_stack_get(ctx, this_val, class_id);
    if (!s)
        return JS_EXCEPTION;
    /* Use the intrinsic prototype directly so tampering with the global
       binding or subclassing cannot redirect move(). */
    new_obj = JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id],
                                     class_id);
    if (JS_IsException(new_obj))
        return JS_EXCEPTION;
    ns = js_mallocz(ctx, sizeof(*ns));
    if (!ns) {
        JS_FreeValue(ctx, new_obj);
        return JS_EXCEPTION;
    }
    JS_SetOpaqueInternal(new_obj, ns);
    /* Transfer resources to new stack */
    ns->resources = s->resources;
    ns->resource_count = s->resource_count;
    ns->resource_capacity = s->resource_capacity;
    /* Reset original stack */
    s->resources = NULL;
    s->resource_count = 0;
    s->resource_capacity = 0;
    s->disposed = true;
    return new_obj;
}

static JSValue js_disposable_stack_get_disposed(JSContext *ctx,
                                                JSValueConst this_val, int class_id)
{
    JSDisposableStack *s;

    s = JS_GetOpaque2(ctx, this_val, class_id);
    if (!s)
        return JS_EXCEPTION;
    return js_bool(s->disposed);
}

static const JSCFunctionListEntry js_disposable_stack_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("adopt", 2, js_disposable_stack_adopt, JS_CLASS_DISPOSABLE_STACK ),
    JS_CFUNC_MAGIC_DEF("defer", 1, js_disposable_stack_defer, JS_CLASS_DISPOSABLE_STACK ),
    JS_CFUNC_STEP_DEF("dispose", 0, STEPDEF_DISPOSE_SYNC ),
    JS_CFUNC_MAGIC_DEF("move", 0, js_disposable_stack_move, JS_CLASS_DISPOSABLE_STACK ),
    JS_CFUNC_STEP_DEF("use", 1, STEPDEF_DISPOSABLE_USE ),
    JS_CGETSET_MAGIC_DEF("disposed", js_disposable_stack_get_disposed, NULL, JS_CLASS_DISPOSABLE_STACK ),
    JS_ALIAS_DEF("[Symbol.dispose]", "dispose" ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DisposableStack", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_async_disposable_stack_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("adopt", 2, js_disposable_stack_adopt, JS_CLASS_ASYNC_DISPOSABLE_STACK ),
    JS_CFUNC_MAGIC_DEF("defer", 1, js_disposable_stack_defer, JS_CLASS_ASYNC_DISPOSABLE_STACK ),
    JS_CFUNC_STEP_DEF("disposeAsync", 0, STEPDEF_DISPOSE_ASYNC ),
    JS_CFUNC_MAGIC_DEF("move", 0, js_disposable_stack_move, JS_CLASS_ASYNC_DISPOSABLE_STACK ),
    JS_CFUNC_STEP_DEF("use", 1, STEPDEF_ASYNC_DISPOSABLE_USE ),
    JS_CGETSET_MAGIC_DEF("disposed", js_disposable_stack_get_disposed, NULL, JS_CLASS_ASYNC_DISPOSABLE_STACK ),
    JS_ALIAS_DEF("[Symbol.asyncDispose]", "disposeAsync" ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncDisposableStack", JS_PROP_CONFIGURABLE),
};

