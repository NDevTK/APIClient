/* LengthOfArrayLike as a RESUMABLE sub-sequence. Both halves can run user code: the READ can be an accessor or a
   proxy trap, and ToLength on an object result runs a valueOf/toString/@@toPrimitive. From a C prologue a loop in
   any of them hits "@WHY loop preempted in a NON-coroutine activation" — its C entry never became a flow base. So
   the read is a GETPROP step and the ToLength is a TOPRIMITIVE step; WHICH shape the read turns out to be is the
   driver's business, not this function's.
   The caller spends ONE stage here and re-enters with each step's result until this returns 0:
     0   = *plen is filled, the sub-sequence is finished and reset
     5/6 = the caller must return that step code; it will be re-entered at the same stage
     -1  = threw. */
enum { LEN_PH_START = 0, LEN_PH_GOT, LEN_PH_PRIM, LEN_PH_UNKNOWN };

/* HOW LONG IS AN UNKNOWN ARRAY-LIKE — the one place opaque ITERATION is decided, because every walk that
 * iterates by index (Array.from over an array-like, map/forEach/reduce/join/flat/sort, Web IDL's sequence<>)
 * reaches its bound through this one sub-sequence. The read produced a value that is unknown external input, so
 * ToLength has no answer: it reaches the conversion boundary's DCHECK, which is where `Array.from(document.cookie)`
 * aborted the whole document.
 *
 * A length is not one fact with one answer — every non-negative integer is a world the program can be in, and
 * the code behind `if (items.length)` and the value at each index differ between them. So the walk's bound is
 * asked as a CHAIN OF PER-POSITION OUTCOME FORKS: at n, "is this collection longer than n?". Position n and
 * position n+1 are DIFFERENT predicates — the operation string carries n, which is exactly what solver_outcome's
 * key space is for — so neither is decided by the other's answer and the flow that keeps saying "longer" keeps
 * forking rather than replaying one answer forever.
 *
 * OUTCOME 0 IS "no, the length is n", the completion a run with no forking policy takes: at n = 0 that is the
 * empty walk, a concrete terminating path, which is what an @S candidate re-fire on its way to a sink needs.
 * OUTCOME 1 is "longer", and it asks again at n + 1. There is NO BOUND on n and there must not be one — a cap, a
 * seen-set or a same-length fixpoint would delete every world past it. What makes this behave like a finite walk
 * is the WFQ: the flow that keeps answering "longer" emits nothing new, so work aging sinks it below every flow
 * that does, and it is outranked and paged rather than stopped.
 *
 * The probe cursor and the operation string live on the HEADER, not in this function's locals: the machine
 * suspends at every ask (the sibling's snapshot is taken there) and its C locals are gone when it resumes. The
 * unknown itself rides `coerce`, which the clone dups and the teardown frees, so a machine abandoned mid-chain
 * leaks nothing. */
/* The completion position `h->len_probe`'s ask reaches on the unknown's example: §7.1.20 ToLength of a
   primitive example, run here because a primitive's conversion runs no page code. Longer (1) below it, the
   length (0) at it; past it, and for an absent or Object example, unstated. */
static int step_length_unknown_real(JSContext *ctx, JSStepHdr *h)
{
    JSValue ex = g_concolic.example(ctx, h->coerce);
    int64_t len;
    int real = JS_OUTCOME_REAL_UNSTATED;

    if (!JS_IsUndefined(ex) && !JS_IsObject(ex) && !JS_IsSymbol(ex) && !JS_IsBigInt(ex)) {
        int ok = JS_ToLengthFree(ctx, &len, js_dup(ex));
        DCHECK(ok == 0, "ToLength of a primitive example other than a Symbol or a BigInt threw");
        real = h->len_probe < len ? 1 : h->len_probe == len ? 0 : JS_OUTCOME_REAL_UNSTATED;
    }
    JS_FreeValue(ctx, ex);
    return real;
}

static int step_length_unknown(JSContext *ctx, JSStepHdr *h, int64_t *plen)
{
    for (;;) {
        int arm, r;
        DCHECK(g_concolic.is(h->coerce),
               "the unknown-length chain resumed holding something that is not unknown input — the operand it "
               "forks over has to be the value the length read actually produced");
        snprintf(h->len_op, sizeof h->len_op, "LengthOfArrayLike>%d", h->len_probe);
        r = step_fork_run(ctx, h, h->coerce, h->len_op, 2, step_length_unknown_real(ctx, h), &arm);
        if (r)
            return r;                     /* JS_STEP_FORK: the driver snapshots the sibling and re-enters here */
        if (arm == 0) {                   /* the length is exactly this many */
            *plen = h->len_probe;
            JS_FreeValue(ctx, h->coerce);
            h->coerce = JS_UNDEFINED;
            h->len_probe = 0;
            h->len_phase = LEN_PH_START;
            return 0;
        }
        h->len_probe++;                   /* longer: the next position is its own predicate */
        CHECK(h->len_probe > 0, "the unknown-length probe wrapped — the frontier's floor is RAM and disk, not "
                                "an integer width, so this cursor has to be wide enough to be one");
    }
}

/* the tail shared by both entries into ToLength: `v` is the raw length value, owned and consumed. */
static int step_length_value(JSContext *ctx, JSStepHdr *h, JSValue v, int64_t *plen,
                             JSValue **out_cb, int *out_argc)
{
    if (unlikely(g_concolic.is(v))) {
        h->coerce = v;             /* owned across the whole chain of asks */
        h->len_probe = 0;
        h->len_phase = LEN_PH_UNKNOWN;
        return step_length_unknown(ctx, h, plen);
    }
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
        h->coerce = v;             /* owned until the primitive comes back */
        h->cb_coerce[0] = v;       /* borrowed view — the request buffer never owns */
        *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
        h->len_phase = LEN_PH_PRIM;
        return 5;
    }
    h->len_phase = LEN_PH_START;
    return JS_ToLengthFree(ctx, plen, v);
}

/* ArraySpeciesCreate (7.3.22) as a RESUMABLE sub-sequence, the same shape as LengthOfArrayLike above. THREE of
   its steps run user code — Get(O,"constructor"), Get(C,@@species) and the Construct — so all three are steps
   (6, 6, 4) and the whole thing suspends wherever the page's code does. `h->coerce` carries the constructor
   across each suspension, because the machine's C locals are gone when it resumes.
     0   = *pout holds the created array
     4/6 = the caller must return that step code; it will be re-entered at the same stage
     -1  = threw. */
enum { SPC_START = 0, SPC_CTOR, SPC_SPECIES, SPC_CONSTRUCT };

/* Steps 7-9, shared by "the constructor was not an object" and "the @@species read settled": undefined means a
   plain Array of that length, and ANYTHING else is Construct'ed — where a non-constructor throws, which IS step
   9's TypeError. Consumes `ctor`. */
static int step_species_finish(JSContext *ctx, JSStepHdr *h, JSValue ctor, JSValueConst len_val,
                               JSValue *pout, JSValue **out_cb, int *out_argc)
{
    if (JS_IsUndefined(ctor)) {
        h->spc_phase = SPC_START;
        *pout = js_array_constructor(ctx, JS_UNDEFINED, 1, &len_val);
        return JS_IsException(*pout) ? -1 : 0;
    }
    h->coerce = ctor;                     /* owned across the Construct */
    h->cb_coerce[0] = ctor;               /* borrowed view */
    h->cb_coerce[1] = (JSValue)len_val;   /* a number: no ownership to transfer */
    *out_cb = h->cb_coerce; *out_argc = 1;
    h->spc_phase = SPC_CONSTRUCT;
    return 4;
}

static int step_species_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSValue in,
                            JSValueConst len_val, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    JSValue ctor;
    switch (h->spc_phase) {
    case SPC_START: {
        int res = js_is_array(ctx, obj);
        JS_FreeValue(ctx, in);
        if (res < 0) return -1;
        if (!res) {   /* not an array: a plain Array of that length, no user code at all */
            *pout = js_array_constructor(ctx, JS_UNDEFINED, 1, &len_val);
            return JS_IsException(*pout) ? -1 : 0;
        }
        h->cb_coerce[0] = (JSValue)obj;   /* borrowed: the machine holds the receiver */
        *out_cb = h->cb_coerce; *out_argc = (int)JS_ATOM_constructor;
        h->spc_phase = SPC_CTOR;
        return 6;
    }
    case SPC_CTOR:
        ctor = in;
        if (JS_IsConstructor(ctx, ctor)) {
            /* legacy web compatibility: another realm's %Array% means "no species" */
            JSContext *realm = JS_GetFunctionRealm(ctx, ctor);
            if (!realm) { JS_FreeValue(ctx, ctor); return -1; }
            if (realm != ctx && js_same_value(ctx, ctor, realm->array_ctor)) {
                JS_FreeValue(ctx, ctor);
                ctor = JS_UNDEFINED;
            }
        }
        if (JS_IsObject(ctor)) {
            h->coerce = ctor;                 /* owned across the @@species read */
            h->cb_coerce[0] = ctor;           /* borrowed view */
            *out_cb = h->cb_coerce; *out_argc = (int)JS_ATOM_Symbol_species;
            h->spc_phase = SPC_SPECIES;
            return 6;
        }
        /* NOT an object: step 4's @@species read is skipped, but steps 7-9 still apply — `a.constructor = null`
           reaches step 9 and throws, it does not quietly get a plain Array. Same tail as the species result. */
        return step_species_finish(ctx, h, ctor, len_val, pout, out_cb, out_argc);
    case SPC_SPECIES:
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        ctor = JS_IsNull(in) ? (JS_FreeValue(ctx, in), JS_UNDEFINED) : in;
        return step_species_finish(ctx, h, ctor, len_val, pout, out_cb, out_argc);
    default:
        DCHECK(h->spc_phase == SPC_CONSTRUCT, "ArraySpeciesCreate resumed in an unknown phase");
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        h->spc_phase = SPC_START;
        *pout = in;
        return 0;
    }
}

/* TypedArraySpeciesCreate (23.2.4.3) as a RESUMABLE sub-sequence — the same shape and the same phases as
   ArraySpeciesCreate above. Three of its steps run the page's code: Get(O,"constructor"), Get(C,@@species) and
   the Construct. js_typed_array___speciesCreate performed all three from C, so `%TypedArray%[@@species]` set to
   a constructor with a loop in it had no flow base — and once a builtin constructor is a STEP MACHINE that C
   path cannot run it at all, which is what `ta.map(fn)` with the species set to Array proved.
   What differs from the array form is only the two ends: the default is the exemplar's OWN intrinsic
   constructor, and the constructed value is validated as a typed array long enough for the request
   (TypedArrayCreateFromConstructor). The middle is identical, which is why the phases are shared.
     0   = *pout holds the created typed array
     4/6 = the caller must return that step code; it will be re-entered at the same stage
     -1  = threw. */
static int js_typed_array_get_length_unsafe(JSContext *ctx, JSValueConst obj);
static int js_array_private_push(JSContext *ctx, JSValueConst arr, JSValueConst v);
static int js_array_private_pop(JSContext *ctx, JSValueConst arr);

static int step_ta_species_finish(JSContext *ctx, JSStepHdr *h, JSValue ctor, JSValueConst len_val,
                                  int class_id, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    if (JS_IsUndefined(ctor)) {
        h->spc_phase = SPC_START;
        *pout = js_typed_array_constructor(ctx, JS_UNDEFINED, 1, &len_val, class_id);
        return JS_IsException(*pout) ? -1 : 0;
    }
    if (!JS_IsConstructor(ctx, ctor)) {   /* SpeciesConstructor step 7 */
        JS_FreeValue(ctx, ctor);
        h->spc_phase = SPC_START;
        JS_ThrowTypeError(ctx, "not a constructor");
        return -1;
    }
    h->coerce = ctor;                     /* owned across the Construct */
    h->cb_coerce[0] = ctor;               /* borrowed view */
    h->cb_coerce[1] = (JSValue)len_val;   /* a number: no ownership to transfer */
    *out_cb = h->cb_coerce; *out_argc = 1;
    h->spc_phase = SPC_CONSTRUCT;
    return 4;
}

static int step_ta_species_run(JSContext *ctx, JSStepHdr *h, JSValueConst exemplar, JSValue in,
                               JSValueConst len_val, int class_id, bool require_mutable,
                               JSValue *pout, JSValue **out_cb, int *out_argc)
{
    JSValue ctor;
    switch (h->spc_phase) {
    case SPC_START:
        JS_FreeValue(ctx, in);
        h->cb_coerce[0] = (JSValue)exemplar;   /* borrowed: the machine holds the receiver */
        *out_cb = h->cb_coerce; *out_argc = (int)JS_ATOM_constructor;
        h->spc_phase = SPC_CTOR;
        return 6;
    case SPC_CTOR:
        ctor = in;
        if (JS_IsUndefined(ctor))   /* SpeciesConstructor step 2 */
            return step_ta_species_finish(ctx, h, JS_UNDEFINED, len_val, class_id, pout, out_cb, out_argc);
        if (!JS_IsObject(ctor)) {   /* step 3 */
            JS_FreeValue(ctx, ctor);
            h->spc_phase = SPC_START;
            JS_ThrowTypeError(ctx, "not an object");
            return -1;
        }
        h->coerce = ctor;                 /* owned across the @@species read */
        h->cb_coerce[0] = ctor;           /* borrowed view */
        *out_cb = h->cb_coerce; *out_argc = (int)JS_ATOM_Symbol_species;
        h->spc_phase = SPC_SPECIES;
        return 6;
    case SPC_SPECIES:
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        ctor = (JS_IsNull(in) || JS_IsUndefined(in)) ? (JS_FreeValue(ctx, in), JS_UNDEFINED) : in;
        return step_ta_species_finish(ctx, h, ctor, len_val, class_id, pout, out_cb, out_argc);
    default: {
        int new_len;
        int64_t want;
        DCHECK(h->spc_phase == SPC_CONSTRUCT, "TypedArraySpeciesCreate resumed in an unknown phase");
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        h->spc_phase = SPC_START;
        /* TypedArrayCreateFromConstructor's validation, on the object the page's constructor returned. */
        new_len = js_typed_array_get_length_unsafe(ctx, in);
        if (new_len < 0) goto fail;
        if (require_mutable && typed_array_is_immutable(JS_VALUE_GET_OBJ(in))) {
            JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
            goto fail;
        }
        if (JS_ToLengthFree(ctx, &want, js_dup(len_val))) goto fail;
        if (new_len < want) {
            JS_ThrowTypeError(ctx, "TypedArray length is too small");
        fail:
            JS_FreeValue(ctx, in);
            return -1;
        }
        *pout = in;
        return 0;
    }
    }
}

