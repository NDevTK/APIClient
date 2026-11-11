/* ---- for-in's key collection, 14.7.5.9 EnumerateObjectProperties ----

   build_for_in_iterator ran the whole enumeration from C: [[GetPrototypeOf]] per link (a Proxy's
   `getPrototypeOf` trap) and [[OwnPropertyKeys]] + one [[GetOwnProperty]] per key per link (its `ownKeys` and
   `getOwnPropertyDescriptor` traps). A loop in any of them preempted in an activation with no flow base, and
   `for (k in proxy)` reached ALL THREE that way — the exotic guard added for the prototype PROBE was applied to
   each link's prototype and never to the receiver itself, so the receiver's own traps were unguarded on both
   the probe's first hop and the key walk under `normal_case`.

   It is a step machine now, driven from OP_for_in_start. The fast path stays C and gets the guard it was
   missing: it is a DIFFERENT ALGORITHM, not a fallback, and its whole precondition — the receiver AND every
   link above it being an ordinary object — is decided without running anything, so the enumeration it computes
   is observationally a shape walk. Anything failing that precondition is simply not this algorithm and takes
   the request-driven walk, where each hop is a suspension point and an endless Proxy chain is a parkable flow
   rather than a hang. */

/* Is this object's [[GetPrototypeOf]], [[OwnPropertyKeys]] and [[GetOwnProperty]] all ordinary? A Proxy is the
   one class whose [[GetPrototypeOf]] is a trap, and it also carries both key-walk exotics; a class that
   overrides either of those two can compute keys, which is user code by another name. */
static bool for_in_is_ordinary(JSContext *ctx, JSObject *p)
{
    const JSClassExoticMethods *em;
    if (p->class_id == JS_CLASS_PROXY)
        return false;
    if (!p->is_exotic)
        return true;
    em = ctx->rt->class_array[p->class_id].exotic;
    return !(em && (em->get_own_property_names || em->get_own_property));
}

/* THE TWO COMPLETIONS OF ONE STEP OF A PROTOTYPE-CHAIN WALK WHOSE ONLY STOPPING TEST IS `is null`, over a
   link that is unknown external input. It is the SIBLING of PROTO_LINK_END/MATCH/MORE and deliberately not
   that numbering: those three belong to a walk that ALSO compares each link against an object it is looking
   for, and a walk carrying no such comparison has no MATCH to number. Declaring three completions where two
   are feasible is the same defect as declaring two where three are — a machine stating a world its algorithm
   cannot reach — so each site counts from what the PAGE can observe at ITS ask and never from the arms of a
   neighbouring algorithm's prose.
   ZERO ENDS THE WALK, forced rather than chosen and for the identical reason the three-way numbering puts its
   own END at 0: the driver takes outcome 0 where no forking policy answers, so a numbering whose 0 were MORE
   would walk an unknown chain for ever in exactly the run that has no scheduler to park it.
   SHARING A NUMBERING IS NOT SHARING A QUESTION. The constraint key is (operand, operation, completion), so
   every site still spells an operation string of its own — see PROTO_LINK_END's note for what goes wrong when
   two walks that ask different things about one link agree on a name. */
#define PROTO_CHAIN_ENDS 0   /* the chain ends at this link: this walk is over */
#define PROTO_CHAIN_MORE 1   /* there is another link: ask this one for ITS [[GetPrototypeOf]] */

/* THE TYPE CONSTRAINT ON A CHAIN LINK, ASSERTED WHERE THE LINK IS TAKEN. A walk that hands its enumeration or
   its comparison something that is not an Object and not null has a broken invariant whatever produced it, so
   the check belongs at the one line each walk turns a delivery INTO a link.
   §6.1.7.2's general statement and not §10.1.1's, because a link may be a Proxy and answer through §10.5.1
   while §10.1.1 is the ORDINARY object's method. EXPANDED AT EACH SITE rather than called: a DCHECK inside a
   helper stamps the helper's line for every caller, and four walks share this one, so the abort would name an
   action with no object. Asked only on the entry that CARRIES a link — a fork's re-entry carries none, which
   is step_fork_pending's question and not this one's. */
#define STEP_CHECK_PROTO_LINK(v)                                                                              \
    DCHECKF(JS_IsNull(v) || JS_IsObject(v),                                                                   \
            "[[GetPrototypeOf]] answered a chain link that is neither an Object nor null, tag %d "            \
            "(quickjs.h JS_TAG_*: 2 NULL, 3 UNDEFINED). "                                                     \
            "ECMAScript §6.1.7.2 \"Object Internal Methods and Internal Slots\" says it "                      \
            "\"returns either a normal completion containing either an Object or null, or a throw completion\"" \
            " — so either the step-18 delivery answered what no [[GetPrototypeOf]] may, or this arm ran on an " \
            "entry carrying NO link that step_fork_pending did not exclude",                                  \
            (int)JS_VALUE_GET_TAG(v))

typedef struct JSForIn {
    JSStepHdr hdr;         /* MUST be first — enforced by STEP_STATE_HDR_FIRST below */
    JSValue enum_obj;      /* the JS_CLASS_FOR_IN_ITERATOR being built (owned) */
    JSValue cur;           /* the prototype-chain link being walked (owned) */
    JSValue result;        /* DONE: the iterator (owned) */
    JSEnumKeys *ek;        /* the current link's key walk (owned) */
} JSForIn;

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). The chain walk had a
   PRIVATE `phase` cursor beside the stage, and the machine parks in all three of its values — so the stage alone
   said only "somewhere in the chain walk", which is exactly the approximate rest point the declaration exists to
   remove. The phases ARE the stages now and the field is gone.
   14.7.5.9 EnumerateObjectProperties is one prose step; the operations it REQUIRES an implementation to perform
   are 14.7.5.10.2.1's, and those are what this parks on. (The prose here called EnumerateObjectProperties
   14.7.5.10, which is the containing "For-In Iterator Objects" section, not the operation.) */
#define FORIN_STAGES(X) \
    X(FI_CREATE, "14.7.5.9 step 1, built as 14.7.5.10.1 CreateForInIterator(object) steps 1-6 - and the " \
                 "ordinary-object collection, which reaches no internal method the page can trap") \
    X(FI_LINK,   "14.7.5.10.2.1 step 5 (the Repeat head) and step 5.f (a null object ends the enumeration)") \
    X(FI_KEYS,   "14.7.5.10.2.1 steps 5.a.i and 5.b.iii.1 (keys is object.[[OwnPropertyKeys]](); desc is " \
                 "object.[[GetOwnProperty]](r) for each key)") \
    X(FI_PROTO,  "14.7.5.10.2.1 steps 5.c-5.e (object is object.[[GetPrototypeOf]]())")
enum { FORIN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_for_in_steps[] = { FORIN_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The fast path, in full: the enumeration is exactly the receiver's own enumerable string keys, which holds
   when nothing above it contributes any. Returns 1 = `it` is filled and the walk is over, 0 = not this
   algorithm, -1 = threw. Runs NO user code — that is the precondition, not a hope. */
static int for_in_fast(JSContext *ctx, JSValueConst obj, JSForInIterator *it, JSValueConst enum_obj)
{
    JSPropertyEnum *tab_atom;
    uint32_t tab_atom_count;
    JSValue obj1;
    JSObject *p;
    int i;

    p = JS_VALUE_GET_OBJ(obj);
    if (!for_in_is_ordinary(ctx, p))
        return 0;
    obj1 = js_dup(obj);
    for (;;) {
        JSObject *pp;
        obj1 = JS_GetPrototypeFree(ctx, obj1);   /* the link it came from is ordinary: no trap */
        if (JS_IsNull(obj1))
            break;
        if (JS_IsException(obj1))
            return -1;
        pp = JS_VALUE_GET_OBJ(obj1);
        if (!for_in_is_ordinary(ctx, pp)) { JS_FreeValue(ctx, obj1); return 0; }
        if (JS_GetOwnPropertyNamesInternal(ctx, &tab_atom, &tab_atom_count, pp,
                                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            JS_FreeValue(ctx, obj1);
            return -1;
        }
        js_free_prop_enum(ctx, tab_atom, tab_atom_count);
        if (tab_atom_count != 0) { JS_FreeValue(ctx, obj1); return 0; }   /* something above contributes */
        /* an ordinary prototype chain cannot be cyclic — JS_SetPrototype rejects that — so this terminates;
           the interrupt check is the runtime's, not a bound on the enumeration. */
        if (js_poll_interrupts(ctx)) { JS_FreeValue(ctx, obj1); return -1; }
    }

    if (p->fast_array) {
        JSShape *sh = p->shape;
        JSShapeProperty *prs;
        bool named = false;
        for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
            if (prs->flags & JS_PROP_ENUMERABLE) { named = true; break; }
        }
        if (!named) {
            /* for a fast array the indices are the enumeration, so only their count is stored */
            it->is_array = true;
            it->array_length = p->u.array.count;
            return 1;
        }
    }
    /* The SIBLING walk above tests for_in_is_ordinary before asking; this one is reached only after the same
       question was settled for `p`, and saying so as an assert is what retires it from the C-side list rather
       than leaving it to be re-derived. An enumerability walk on a Proxy is its ownKeys trap plus a
       getOwnPropertyDescriptor per key — all of it the page's code, none of it with a flow base here. */
    DCHECK(for_in_is_ordinary(ctx, p),
           "a for-in enumeration walked a non-ordinary object from C — a Proxy's ownKeys and per-key "
           "getOwnPropertyDescriptor would run with no flow base; route it onto the request path");
    if (JS_GetOwnPropertyNamesInternal(ctx, &tab_atom, &tab_atom_count, p,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < (int)tab_atom_count; i++)
        JS_SetPropertyInternal(ctx, enum_obj, tab_atom[i].atom, JS_NULL, 0);
    js_free_prop_enum(ctx, tab_atom, tab_atom_count);
    return 1;
}

static int js_for_in_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSForIn *s = st;
    int r, arm;

    if (s->hdr.stage == FI_CREATE) {
        JSValue obj;
        JSForInIterator *it;
        JSObject *p;
        uint32_t tag;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->enum_obj = JS_UNDEFINED; s->cur = JS_UNDEFINED; s->result = JS_UNDEFINED;

        obj = js_dup(step_arg(&s->hdr, 0));
        tag = JS_VALUE_GET_TAG(obj);
        if (tag != JS_TAG_OBJECT && tag != JS_TAG_NULL && tag != JS_TAG_UNDEFINED) {
            obj = JS_ToObjectFree(ctx, obj);   /* a primitive wrapper: nothing to run */
            if (JS_IsException(obj)) return -1;
        }
        /* ZEROED, not js_malloc'd: `pending` is a field the C body never had, and an allocation that leaves
           any field undefined makes adding one a silent bug at a site nobody edits. Zero IS this struct's
           resting state — JS_ATOM_NULL is 0 — so the obligation is discharged by construction. */
        it = js_mallocz(ctx, sizeof(*it));
        if (!it) { JS_FreeValue(ctx, obj); return -1; }
        s->enum_obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_FOR_IN_ITERATOR);
        if (JS_IsException(s->enum_obj)) {
            s->enum_obj = JS_UNDEFINED;
            js_free(ctx, it);
            JS_FreeValue(ctx, obj);
            return -1;
        }
        it->is_array = false;
        it->obj = obj;            /* the iterator owns it from here */
        it->idx = 0;
        it->pending = JS_ATOM_NULL;
        p = JS_VALUE_GET_OBJ(s->enum_obj);
        p->u.for_in_iterator = it;

        if (tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED) {
            s->result = s->enum_obj; s->enum_obj = JS_UNDEFINED;
            return 0;
        }
        r = for_in_fast(ctx, obj, it, s->enum_obj);
        if (r < 0) return -1;
        if (r > 0) {
            s->result = s->enum_obj; s->enum_obj = JS_UNDEFINED;
            return 0;
        }
        s->cur = js_dup(obj);
        s->hdr.stage = FI_LINK;
    }

    for (;;) {
        switch (s->hdr.stage) {
        case FI_LINK:
            if (JS_IsNull(s->cur)) {
                JS_FreeValue(ctx, cb_result);
                s->result = s->enum_obj; s->enum_obj = JS_UNDEFINED;
                return 0;
            }
            /* 14.7.5.10.2.1 step 5.a.i's own-key list for THIS link. Non-enumerable keys are collected too: one of them
               SHADOWS an enumerable key of the same name further up, so dropping it here would enumerate a
               property the spec hides. */
            s->ek = js_mallocz(ctx, sizeof(*s->ek));
            if (!s->ek) { JS_FreeValue(ctx, cb_result); return -1; }
            js_enum_keys_init(s->ek);
            s->ek->obj = js_dup(s->cur);
            s->hdr.stage = FI_KEYS;
            continue;

        case FI_KEYS: {
            uint32_t k;
            r = js_enum_keys_run(ctx, &s->hdr, s->ek, cb_result, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r != 0) return r;
            for (k = 0; k < s->ek->kept; k++) {
                /* the properties are defined NON-CONFIGURABLE, so the SHALLOWEST definition of a name wins and
                   every later one silently fails — which is exactly the shadowing rule. */
                JS_DefinePropertyValue(ctx, s->enum_obj, s->ek->atoms[k].atom, JS_NULL,
                                       s->ek->atoms[k].is_enumerable ? JS_PROP_ENUMERABLE : 0);
            }
            js_enum_keys_free(ctx, s->ek);
            js_free(ctx, s->ek); s->ek = NULL;
            /* the next link: [[GetPrototypeOf]], which on a Proxy is its trap. */
            s->hdr.stage = FI_PROTO;
            s->hdr.cb_coerce[0] = s->cur;   /* borrowed: the machine holds it across the request */
            *out_cb = s->hdr.cb_coerce; *out_argc = 0;
            return 18;
        }

        default:
            DCHECK(s->hdr.stage == FI_PROTO, "the for-in key walk resumed in no phase");
            /* THREE WAYS TO ARRIVE HERE AND ONLY ONE OF THEM CARRIES A LINK — step 5.c's request answering,
               whose result IS the link; this stage's own fork being ANSWERED; and the SIBLING that fork
               minted, resuming at the ask. The last two carry the driver's filler rather than a value from
               the algorithm, and taking it as the link overwrites the very operand the fork was asked about.
               step_fork_pending is what tells them apart and `fork_phase` is NOT — see its definition. This
               arm read the sibling as a link and handed JS_UNDEFINED to the enumeration seam, which is where
               it aborted; js_proto_chain_step, js_lookup_acc_step and js_instanceof_step had the same entry
               and failed silently. */
            if (!step_fork_pending(&s->hdr)) {
                step_adopt_link(ctx, &s->cur, &cb_result);
                if (JS_IsException(s->cur)) { s->cur = JS_UNDEFINED; return -1; }
                STEP_CHECK_PROTO_LINK(s->cur);
            }
            /* ASKED OF THE LINK THAT ARRIVED AND NOT OF THE STARTING OBJECT, which is the whole of why this
               sits here rather than at FI_LINK. `for (k in rec)` over an unknown RECORD is supported: its own
               keys come from this class's own enumeration seam. Its PROTOTYPE is a derivation with no example
               and is never null, so step 5.f's `If object is null` — this Repeat's ONLY stopping test — is a
               C test against a value that is an ordinary Object at the C level, and the walk would take the
               keep-walking arm at every link and never terminate.
               TWO COMPLETIONS, COUNTED FROM WHAT THE PAGE OBSERVES RATHER THAN FROM THE PROSE. What a
               `for..in` can see here is whether a further key ever arrives: the enumeration is over with
               exactly the keys already collected, or this link joins it and contributes its own. There is no
               third. This Repeat holds no comparison against anything, so the MATCH arm of the three-way
               numbering names no world here; and the world in which the chain is longer than any particular
               length is the MORE arm rather than a completion beside it.
               THE CRASH THAT STOOD HERE PRESCRIBED THAT THREE-WAY NUMBERING, and its spec half was right
               while its remedy clause was not — a completion count borrowed from a neighbouring algorithm
               instead of derived here. Recorded at the site: a remedy clause is a hypothesis about this tree
               and the reader who acts on one acts on it once. */
            if (js_value_is_concolic(s->cur)) {
                JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
                /* `real` UNSTATED: the derived link carries no example, so nothing this run observed says
                   which completion a real session reaches. */
                r = step_fork_run(ctx, &s->hdr, s->cur, "%ForInIteratorPrototype%.next step 5.f", 2,
                                  JS_OUTCOME_REAL_UNSTATED, &arm);
                if (r) return r;
                if (arm == PROTO_CHAIN_ENDS) {                         /* step 5.f */
                    s->result = s->enum_obj; s->enum_obj = JS_UNDEFINED;
                    return 0;
                }
                /* PROTO_CHAIN_MORE — steps 5.d and 5.e, then the Repeat head at 5.a over THIS link, whose own
                   keys the enumeration seam answers under a question of its own. The next link's ask is made
                   under ITS identity, so an unknown chain is a chain of questions and not one repeated. */
            }
            s->hdr.stage = FI_LINK;
            continue;
        }
    }
}

static void js_enum_keys_visit(JSContext *ctx, void *elem, JSStepVisit *v);

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The chain walk asks each link for its own keys, which a
   Proxy can trap, so a fork lands with the cursor live — the same one-element array shape a JSON walk frame's
   cursor uses. */
static void js_for_in_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSForIn *s = st;
    v->array(ctx, (void **)&s->ek, sizeof(JSEnumKeys), 1, 1, js_enum_keys_visit);
    v->val(ctx, &s->enum_obj);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->result);
}

static JSValue js_for_in_fini(JSContext *ctx, void *st, bool take_result)
{
    JSForIn *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

