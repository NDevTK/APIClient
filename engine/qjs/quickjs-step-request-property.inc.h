/* js_call_c_function switches the CURRENT REALM to the callee's before running a C builtin, and a step machine is
   the same builtin — without this `new otherRealm.Function("")` builds its function, and its prototype, out of
   the CALLER's intrinsics. Seven realm tests in built-ins/Function are what noticed; every machine was running
   in the wrong realm, the Function constructor is just the first one for which that is observable. A CLOSURE
   step machine (a Promise reaction, class JS_CLASS_C_FUNCTION_DATA) names the realm recorded on its own data
   record — the same one js_call_c_function_data now switches to. */
/* THE REALM A BUILTIN RUNS IN, from the callee alone. Split out of step_realm because a step machine is not the
   only machine that IS a builtin: the Iterator.prototype eager terminals are consume machines, and they were
   throwing their TypeErrors and building their arrays out of the CALLER's intrinsics —
   `otherRealm.Iterator.prototype.every.call(iter)` answered with this realm's TypeError, and
   `otherRealm.Iterator.prototype.toArray.call(iter)` with this realm's Array. */
static inline JSContext *js_callee_realm(JSContext *ctx, JSValueConst func)
{
    if (JS_VALUE_GET_TAG(func) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(func);
        if (p->class_id == JS_CLASS_C_FUNCTION && p->u.cfunc.realm)
            return p->u.cfunc.realm;
        if (p->class_id == JS_CLASS_C_FUNCTION_DATA) {
            JSCFunctionDataRecord *cr = p->u.c_function_data_record;
            if (cr && cr->realm)
                return cr->realm;
        }
    }
    return ctx;
}

static inline JSContext *step_realm(JSContext *ctx, const JSStepHdr *h)
{
    return js_callee_realm(ctx, h->func_obj);
}


/* A KEYED property read, Get(O, key), as a RESUMABLE sub-sequence — the primitive every array builtin's loop
   body opens with. Each of them did it with JS_GetPropertyInt64/JS_GetProperty straight out of C, so an index
   accessor or a Proxy `get` trap with a loop in it preempted in an activation with no flow base. The read
   ALWAYS goes to the GETPROP step: WHICH shape it turns out to be — a dense slot, a C getter, a bytecode
   accessor, a proxy trap — is the driver's decision and is made in exactly one place.
   The atom the request carries is BORROWED by the driver, which is why an INDEX read (whose atom is created on
   the spot and, past 2**31, allocated) has to park its atom on the header: the shared teardown releases it, so
   an abandon mid-read cannot leak it.
     0 = *pout holds the element, 6 = the caller must return that step code, -1 = threw. */
enum { GET_PH_START = 0, GET_PH_GOT };

/* WHICH SPEC STEP A STAGE RESTS AT, as a string, for a diagnostic that reports one — the machine's own
   declaration, or a sentence saying the stage is not among them. Written once because three checks now format a
   stage, and a message that walks the list by hand is a third copy of the walk. Dev-only, like every reader it
   has: in release there is no DCHECK left to report a stage. */
#if APICLIENT_DEV
static const char *step_stage_label(const JSStepHdr *h, unsigned stage)
{
    int n = 0;

    while (h->def->steps[n]) n++;
    return stage < (unsigned)n ? h->def->steps[stage]
                               : "(a stage past the end of its declared steps)";
}
#endif

/* A KEYED REQUEST GOES IN FLIGHT — the one point every spelling of one passes, so the fact its ANSWER is
 * checked against is recorded once instead of at the six begin sites. See JSStepHdr::req_stage for why the
 * STAGE is the discriminator the key could never be.
 *
 * IT REFUSES AN OVERLAP ACROSS A STAGE, NOT AN OVERLAP. Two keyed cursors legitimately stand together inside
 * one stage (Web IDL's record<> takes the key list and then reads each key), and that is the shape keys_phase
 * and desc_phase were given their own bytes for. What is refused is the SECOND stage: once a request outlives
 * the stage that issued it, the next stage's request site is the one that will collect its answer, and both
 * sites see a real value belonging to the wrong question. */
static void step_keyed_inflight(JSStepHdr *h)
{
#if APICLIENT_DEV
    if (h->get_atom != JS_ATOM_NULL || h->desc_phase != GET_PH_START || h->keys_phase != GET_PH_START) {
        char why[600];   /* the tail names what to do about it; a truncated DFAIL loses exactly that */

        snprintf(why, sizeof why,
                 "%s issued a keyed request at stage %u (%s) while one issued at stage %u (%s) is still in "
                 "flight — a keyed sub-sequence is answered at the call site that parked on it, so two of them "
                 "spanning a stage boundary means this stage's request site will collect the earlier stage's "
                 "answer, as a real value belonging to another question. A stage that holds a request does "
                 "nothing but hold it: let the outstanding one END before leaving its stage",
                 h->def->algorithm, (unsigned)h->stage, step_stage_label(h, h->stage),
                 (unsigned)h->req_stage, step_stage_label(h, h->req_stage));
        DCHECK(h->req_stage == h->stage, why);
    }
#endif
    h->req_stage = h->stage;
}

/* AND ITS ANSWER ARRIVES AT THE STAGE THAT ASKED — the other half, asserted at the one point every keyed
   request ends. */
static void step_keyed_answered(const JSStepHdr *h)
{
#if APICLIENT_DEV
    char why[600];

    if (h->req_stage == h->stage) return;
    snprintf(why, sizeof why,
             "%s answered a keyed request at stage %u (%s) that was ISSUED at stage %u (%s) — the machine left "
             "the stage that parked on the request, so this call site is collecting another site's answer and "
             "the value it takes is a real one belonging to the other question. The key cannot see this when "
             "the two sites name the same property, and cannot see it at all for an INDEX form, which carries "
             "no key: the stage is what tells them apart",
             h->def->algorithm, (unsigned)h->stage, step_stage_label(h, h->stage),
             (unsigned)h->req_stage, step_stage_label(h, h->req_stage));
    DFAIL(why);
#else
    (void)h;
#endif
}

/* the head both entries share: `atom` is OWNED and moves onto the header. */
static int step_getprop_begin(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom,
                              JSValue **out_cb, int *out_argc)
{
    DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed read is already in flight on this machine's header");
    step_keyed_inflight(h);
    h->get_atom = atom;
    h->cb_coerce[0] = (JSValue)obj;   /* borrowed: the machine holds the receiver across the read */
    *out_cb = h->cb_coerce; *out_argc = (int)atom;
    h->get_phase = GET_PH_GOT;
    return 6;
}

/* ENDS a keyed request and says whether its completion was ABRUPT — the two lines every one of these answer
   paths already ran, plus the one question none of them could ask.
 *
 * ENDING AND REPORTING ARE ONE EVENT, which is why this is a function and not a pair of lines copied around: a
 * request that completed abruptly is over, so the cursor rewinds and the key is released exactly as a normal
 * completion releases them, and the machine is then free to start its next sub-sequence at a clean cursor. The
 * reset therefore comes BEFORE the test.
 * -1 IS THE ANSWER BECAUSE -1 IS WHAT THESE HELPERS ALREADY PROMISED. Every one of them is documented "0 =
 * done, N = the caller must return that step code, -1 = threw", and every caller in this engine was written to
 * that contract — the contract simply had no mechanism behind it, because an abrupt completion never reached
 * the helper at all. Giving it one is the whole fix, and it needs no call site to change: a machine that
 * propagates already returns JS_STEP_ABRUPT on -1, and one whose definition declares catches_abrupt branches
 * there instead. Handing the throw back as a JS_EXCEPTION *value* was the other candidate and is worse: a
 * caller that forgets it flows an exception marker into JS_IsFunction and reports a fabricated TypeError over
 * a live throw, which is a WRONG error rather than a loud one. */
static bool step_keyed_abrupt(JSContext *ctx, JSStepHdr *h, JSValueConst in)
{
    DCHECK(h->get_atom != JS_ATOM_NULL, "a keyed operation was delivered with none in flight on this header");
    step_keyed_answered(h);
    JS_FreeAtom(ctx, h->get_atom);
    h->get_atom = JS_ATOM_NULL;
    h->get_phase = GET_PH_START;
    if (JS_IsException(in)) {
        DCHECK(JS_HasException(ctx),
               "a keyed operation was delivered JS_EXCEPTION with no completion value live in the context");
        return true;
    }
    return false;
}

/* the tail both entries share: the value arrived, so the key is done.

   THE TWO-PHASE CONTRACT, asserted here because violating it is otherwise SILENT. A sub-sequence parks on its
   first call and answers on its second, and the parked key lives on the HEADER — so the second call must be the
   SAME call site. A machine that advances its own phase before the request completes re-enters at the NEXT call
   site instead, which takes the answering branch and receives the OTHER read's value as its own. That produced
   a hang with no diagnostic: `@@asyncIterator`'s parked read was answered at the `@@iterator` call site, which
   consumed it and asked for nothing, forever. `atom` is JS_ATOM_NULL from the INDEX forms, which recompute
   nothing on the answering path and so have no key to check. */
static int step_getprop_done(JSContext *ctx, JSStepHdr *h, JSAtom atom, JSValue in, JSValue *pout)
{
#if APICLIENT_DEV
    /* THE MESSAGE CARRIES WHO AND WHICH KEY, because "a different call site" without them is a bug report with
       the two facts that identify it left out — the algorithm and the pair of keys are exactly what turns this
       from a hunt into a read. */
    if (!(atom == JS_ATOM_NULL || atom == h->get_atom)) {
        char why[420], want[80], got[80];
        const char *aw = JS_AtomGetStr(ctx, want, sizeof want, atom);
        const char *ag = JS_AtomGetStr(ctx, got, sizeof got, h->get_atom);
        snprintf(why, sizeof why,
                 "%s answered a keyed read of `%s` at the call site for `%s` — a DIFFERENT call site than the "
                 "one that parked it, so this receives the other read's value as its own. A two-phase "
                 "sub-sequence keeps its phase until it returns 0; a machine that CATCHES an abrupt delivery "
                 "must still let the request END (call the helper, or take the -1 it reports) before starting "
                 "the next one",
                 h->def->algorithm, ag, aw);
        DFAIL(why);
    }
#endif
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing accessor or `get` trap */
    *pout = in;
    return 0;
}

/* AN ABRUPT COMPLETION IS A DELIVERY, NOT A DEMOLITION — and this is where that used to go wrong.
 *
 * There WAS a step_hdr_request_abandon here, called by the driver before it re-entered a catches_abrupt machine
 * with JS_EXCEPTION: it released the in-flight key and reset get/keys/desc_phase, so the request the machine was
 * parked on ceased to exist a moment before the machine was told about it. That is exactly one request kind
 * answering differently from every other. A CALL reports its throw through the same `out` a return value comes
 * back in (step_call_run's phase is the MACHINE's byte, which the driver cannot reach); a keyed read reported
 * nothing at all, because the driver had already rewound its cursor to "not started" — so the machine's own
 * `step_getprop_run` at the SAME call site took the ASK branch and issued the read a second time. The page's
 * getter threw again, forever. It cost two components a workaround each and one WPT file its entire result:
 * `EventListener-handleEvent`'s "rethrows errors when getting handleEvent" hung, and `observable-from.any.js`
 * went from 48 subtests to a timeout, which discards the ones that had already passed.
 *
 * The delivery is now the ordinary one: the cursor stands, the machine re-enters its helper at the call site
 * that parked, and the helper ENDS the request there — the same lines that end it on the normal path — and
 * REPORTS the abrupt completion as -1, which is what every one of these helpers already documented and what
 * every caller was already written for. Array.fromAsync's case, which the deleted function was written for, is
 * served by that and better: step 12 catches the failing CreateDataPropertyOrThrow AT ITS OWN CALL SITE, where
 * step_defidx_run resets the phase and frees the key, so the close that follows starts its `return` read at a
 * cursor that is clean rather than one the driver guessed was.
 *
 * The invariant this leaves is asserted where every machine passes: see do_step_step. */

/* "I TOOK THE ABRUPT DELIVERY MYSELF" — the one statement the driver could not make and the machine can.
 *
 * Most machines consume an abrupt completion at the call site that parked on it, and the helper there ends the
 * request as part of reporting it. A few state their abrupt handling ONCE for every request they make, because
 * that is what their algorithm says: Array.fromAsync's closure rejects its capability whatever threw, and
 * IfAbruptCloseIterator then reads `return` — a DIFFERENT keyed request, started while the previous one is
 * still on the header. That machine has consumed the delivery, so the request IS over, and only the machine
 * knows it.
 *
 * This is NOT the deleted step_hdr_request_abandon under a new name, and the difference is the whole point of
 * deleting it: that one was the DRIVER deciding, for every machine, that a request had ceased to exist BEFORE
 * the machine was told about it — which is what re-issued the read and hung. This is the machine saying so at
 * the point where it is true, and a machine that forgets does not fail silently: the next keyed read answers at
 * a call site whose key does not match and step_getprop_done names both keys and the algorithm. */
static void step_request_taken(JSContext *ctx, JSStepHdr *h)
{
    if (h->get_atom != JS_ATOM_NULL) {
        JS_FreeAtom(ctx, h->get_atom);
        h->get_atom = JS_ATOM_NULL;
    }
    h->get_phase = GET_PH_START;
    h->keys_phase = GET_PH_START;   /* the own-keys and own-descriptor sub-sequences end with it, for the same */
    h->desc_phase = GET_PH_START;   /* reason: the delivery they were waiting for has been consumed elsewhere */
}

/* a NAMED key, borrowed from the caller (a permanent atom in every current use). */
int step_getprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                     JSValue *pout, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JS_FreeValue(ctx, in);
        return step_getprop_begin(ctx, h, obj, JS_DupAtom(ctx, atom), out_cb, out_argc);
    }
    return step_getprop_done(ctx, h, atom, in, pout);
}

/* A WELL-KNOWN SYMBOL'S ATOM — see quickjs-step.h. A switch rather than a table because a value nobody wrote an
   arm for must abort here rather than index past the end of one. */
JSAtom JS_WellKnownSymbolAtom(JSWellKnownSymbol which)
{
    switch (which) {
    case JS_WKS_ASYNC_ITERATOR: return JS_ATOM_Symbol_asyncIterator;
    case JS_WKS_TO_STRING_TAG: return JS_ATOM_Symbol_toStringTag;
    case JS_WKS_UNSCOPABLES: return JS_ATOM_Symbol_unscopables;
    default:
        DCHECK(which == JS_WKS_ITERATOR, "a well-known symbol was asked for by a name this engine does not map");
        return JS_ATOM_Symbol_iterator;
    }
}

/* %IteratorPrototype% — see quickjs-step.h. It is class_proto[JS_CLASS_ITERATOR] because ES2025 gave the
   intrinsic a constructor (`Iterator`), so the object the iterator helpers hang off IS that class's prototype.
   Returned dup'd: the caller installs it as some object's [[Prototype]] and owns that reference. */
JSValue JS_GetIteratorPrototype(JSContext *ctx)
{
    return js_dup(ctx->class_proto[JS_CLASS_ITERATOR]);
}

/* %AsyncIteratorPrototype% — the same intrinsic one level over, and needed for the same reason: Web IDL
   §3.7.11 states that the ASYNCHRONOUS iterator prototype object of an `async iterable<>` interface has this
   as its [[Prototype]], which is what gives `for await (const chunk of stream)` its @@asyncIterator and the
   async-iterator helpers. It is not a global and not reachable by name, so a host has no way to it at all —
   %IteratorPrototype% at least has a walk from `[][Symbol.iterator]()`, and this one does not. Dup'd. */
JSValue JS_GetAsyncIteratorPrototype(JSContext *ctx)
{
    return js_dup(ctx->async_iterator_proto);
}

/* %Array.prototype.values% — see quickjs-step.h. The same function the arguments object's @@iterator is. */
JSValue JS_GetArrayProtoValues(JSContext *ctx)
{
    return js_dup(ctx->array_proto_values);
}

/* %MapIteratorPrototype% — see quickjs-step.h. It is `class_proto[JS_CLASS_MAP_ITERATOR]`, the object this
   engine's own `Map.prototype.entries` hands its result, and the intrinsic Web IDL §3.7.11.2 names for a
   `maplike<>` interface's iterator. Returned dup'd: the caller installs it as some object's [[Prototype]] and
   owns that reference. */
JSValue JS_GetMapIteratorPrototype(JSContext *ctx)
{
    return js_dup(ctx->class_proto[JS_CLASS_MAP_ITERATOR]);
}

/* 27.1.5.1 CreateAsyncFromSyncIterator ( syncIteratorRecord ), for a HOST performing GetIterator(obj, ASYNC).
 *
 * A host can do every OTHER step of that abstract operation with what quickjs-step.h already exports — the
 * @@asyncIterator and @@iterator reads and the method call are requests like any other. This one step it
 * cannot: the wrapper is an engine intrinsic whose `next` AWAITS the sync result's value, which is what makes
 * `ReadableStream.from([Promise.resolve(1)])` yield 1 rather than the promise. Writing that unwrap in the host
 * would be re-implementing an ECMAScript intrinsic beside the engine's own.
 * `sync_iter` and `next_method` are CONSUMED; *pnext is the wrapper's own `next`, which the caller then calls
 * exactly as it would the sync one. */
JSValue JS_NewAsyncFromSyncIterator(JSContext *ctx, JSValue sync_iter, JSValue next_method, JSValue *pnext)
{
    /* BOTH arguments are consumed here, though the internal form borrows the iterator and takes the method.
       A boundary whose two arguments have two different ownerships is a boundary a caller gets wrong, and this
       one did on its first use — leaking the array iterator, and with it the whole runtime graph. */
    JSValue r = JS_CreateAsyncFromSyncIterator(ctx, sync_iter, next_method, pnext);
    JS_FreeValue(ctx, sync_iter);
    return r;
}

/* [[GetOwnProperty]] AS A REQUEST — see quickjs-step.h. The request is the one the engine's own enumerable-key
   cursor issues (step code 12, the object borrowed in the header's buffer and the key passed as the request's
   argument); this is the two-phase wrapper that lets a HOST machine issue it. */
int step_getownprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                        JSValue *pout, JSValue **out_cb, int *out_argc)
{
    if (h->desc_phase == GET_PH_START) {
        JS_FreeValue(ctx, in);
        step_keyed_inflight(h);
        h->cb_coerce[0] = (JSValue)obj;   /* borrowed: the machine holds the receiver across the request */
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        h->desc_phase = GET_PH_GOT;
        return 12;
    }
    DCHECK(h->desc_phase == GET_PH_GOT,
           "a [[GetOwnProperty]] request was delivered with none in flight on this header");
    /* THIS REQUEST HAS NO KEY CHECK AT ALL — `atom` is what it ASKS about and is recomputed by the caller on
       the answering path, so it says nothing about which site parked. The stage is the whole of its site
       identity, which is why it is the one that most needed one. */
    step_keyed_answered(h);
    h->desc_phase = GET_PH_START;
    if (JS_IsException(in))
        return -1;
    *pout = in;
    return 0;
}

/* THE ASK'S IDENTITY ON THIS WRAPPER — step_tobool_run's `op`, which is NOT the constraint key. The key is the
   predicate's own branch identity, minted by the host over the RECORD, so one machine walking a prototype chain
   asks a DIFFERENT question at every link while naming its call site with this one string. It exists only so
   that an answer cannot be consumed at a call site other than the one that asked; there is exactly one such
   site, because there is exactly one issuer of this request. */
#define OWNKEYS_PRED_OP "ToBoolean of the record's own-key-set predicate"

/* ONE MEMBER, PERFORMED — the position the chain below is standing on, materialised, and the cursor advanced
   past it so the next question is about a position nobody has answered. The two are one operation because
   advancing without materialising would leave a member the flow decided on unnamed for ever. */
static int step_ownkeys_mint(JSContext *ctx, JSStepHdr *h, JSValueConst obj)
{
    int r;

    r = g_concolic.own_key_mint(ctx, obj, h->keys_probe);
    if (r < 0)
        return -1;
    DCHECK(r == 1, "the host declined to materialise a member of a record whose own-key-set predicate IT "
                   "minted — the two answers are about one record, so a host that asks the question owns its "
                   "true arm");
    h->keys_probe++;
    CHECK(h->keys_probe > 0, "the unknown-member cursor wrapped — the frontier's floor is RAM and disk, not an "
                             "integer width, so this cursor has to be wide enough to be one");
    return 0;
}

/* HOW MANY MEMBERS THE RECORD HOLDS, ONE POSITION AT A TIME — the chain the predicate's TRUE arm opens, and the
 * question no single predicate can be. "It holds a member" and "it holds exactly one member" are not the same
 * fact: every count is a world the program can be in, and a wrapper that named one member and stopped would
 * decide `Object.keys(x).length === 2` false for a record no run ever saw — the empty-List fabrication moved
 * one member out rather than removed. So at n the question is "beyond the n members already named, is there
 * another?", position n and position n + 1 are DIFFERENT predicates because the operation string carries n, and
 * there is NO BOUND on n: what makes this behave like a finite enumeration is the WFQ, since the flow that
 * keeps answering "another" emits nothing new and is aged below every flow that does.
 *
 * IT IS THE OUTCOME SEAM WHERE THE FIRST QUESTION IS THE BRANCH SEAM, AND THAT IS NOT ONE CHAIN IN TWO KEY
 * SPACES. The first question has TWO readers — the wrapper below and the record's own §10.1.11 internal method,
 * which reads the arm back when the request reaches it — so it must be keyed by the record's own branch
 * identity, which is the only key both of them can spell. Every question after it has ONE reader, is about a
 * POSITION rather than about the record, and is exactly what solver_outcome's (operand, operation, completion)
 * key space is for — the same shape, spelled the same way, as step_length_unknown's `LengthOfArrayLike>%d`.
 *
 * THE MEMBER IS MATERIALISED, NOT HANDED BACK AS A VALUE. See JSConcolicHooks.own_key_mint for why: the member
 * becomes an ordinary slot whose name is the unknown's own shape, so it arrives in the delivered List as an
 * ordinary String key and every consumer downstream needs nothing at all. The chain therefore runs BEFORE the
 * request is issued — a member materialised afterwards would not be in the List the walk had already built.
 *
 * `h->keys_probe` IS THE COUNT OF MEMBERS ALREADY MATERIALISED, and the outstanding question is always "is there
 * another beyond those?". It is opened at 0 by the branch question's TRUE arm — which IS the question at
 * position 0 — so the first thing this does is perform that arm. The two mint sites are the two
 * AUTHORISATIONS and not one rule written twice: the guarded one spends the answer the caller already has, the
 * loop's spends the one the ask just returned. Placing the ask between them is also what makes the resume point
 * right — a machine re-entered after a fork lands on the ask it parked at, never on a mint it already ran. */
static int step_ownkeys_chain(JSContext *ctx, JSStepHdr *h, JSValueConst obj)
{
    for (;;) {
        int arm, r, w;

        DCHECK(h->keys_probe >= 0, "the unknown-member chain ran with no position — its cursor is the count of "
                                   "members already named and the caller opens it at 0");
        if (h->keys_probe == 0 && step_ownkeys_mint(ctx, h, obj) < 0)
            return -1;
        w = snprintf(h->keys_op, sizeof h->keys_op, "[[OwnPropertyKeys]]>%d", h->keys_probe);
        CHECK(w > 0 && (size_t)w < sizeof h->keys_op,
              "the unknown-member chain's operation string did not fit its buffer — a truncated one keys two "
              "positions to one question, so the chain would answer position n from position m's world");
        r = step_fork_run(ctx, h, obj, h->keys_op, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r)
            return r;                    /* JS_STEP_FORK: the driver snapshots the sibling and re-enters here */
        if (arm == 0)                    /* no member beyond the ones already named: the record is enumerated */
            return 0;
        if (step_ownkeys_mint(ctx, h, obj) < 0)
            return -1;
    }
}

/* [[OwnPropertyKeys]] AS A REQUEST — see quickjs-step.h. The request itself is what the engine's own property
   walk already issues (step code 11, the object borrowed in the header's request buffer); this is the two-phase
   wrapper that lets a HOST machine issue it, which is the half that was missing. It carries no key, so its
   phase byte is the whole of its state — and it is its OWN byte because the record<> conversion that needs this
   reads each key straight afterwards, with a getprop sub-sequence in flight at the same time. */
int step_ownkeys_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSValue in, JSValue *pout,
                     JSValue **out_cb, int *out_argc)
{
    if (h->keys_phase == GET_PH_START) {
        int arm, r;

        JS_FreeValue(ctx, in);   /* never this request's answer: that arrives in the GOT phase below */
        /* THE ENUMERATION'S OWN QUESTION, ASKED HERE BECAUSE HERE IS WHERE IT CAN BE ASKED. A record standing
           for unknown external input that carries no EXAMPLE has an unknown key SET, and answering the empty
           List for it is not a coarse answer but a DECIDED one: it states that the record holds nothing, which
           is a fact no run observed, and it decides `Object.keys(x).length === 0` for the program. The class
           that holds such a record cannot fork over it: §10.1.11 [[OwnPropertyKeys]] ( ) "takes no arguments
           and returns a normal completion containing a List of property keys", and its internal method is
           reached from inside a C activation with no flow base under it, so there is nothing there to snapshot
           a sibling at. A FORK NEEDS A RESUME POINT AND THAT IS THE WHOLE OF WHY THE ASK IS HERE — it is not
           that the arm cannot be expressed in the List. It can: §6.1.7 The Object Type says "A property key is
           either a String or a Symbol." and §6.1.7.3 Invariants of the Essential Internal Methods requires that
           "Each element of the returned List must be a property key", and an unknown key in this engine already
           DENOTES a real String, its own shape (JSConcolicHooks.key_name), so the member the flow decides on is
           materialised under that name and the ordinary walk lists it like any other key.
           THE HOST STATES THE QUESTION AND THIS SEAM ASKS IT. The predicate is minted by the host over the
           record's own identity, so two consumers asking about ONE record ask ONE question; the engine never
           spells that key, which is why this hands the VALUE to the branch seam rather than composing anything.
           THIS ONE QUESTION IS THE BRANCH SEAM AND NOT THE OUTCOME SEAM, because it has TWO readers. A key
           set's emptiness is the same predicate an `if` over it would test, so it is keyed by the value's own
           branch identity — the key the host reads back when the internal method answers the arm this flow
           decided. The outcome seam keys by (operand, operation, completion) and would file this question under
           a second name, which no consumer's fork could then answer. Every question AFTER it has one reader and
           is about a POSITION, and lives at the outcome seam where it belongs — see step_ownkeys_chain.
           HELD ON THE HEADER, NOT IN A C LOCAL, because the ask suspends: step_tobool_run's operand must live
           somewhere the sibling's snapshot carries, and this function's frame is gone when the answer lands. */
        /* THE TAG IS IN THE MESSAGE BECAUSE THE OPERAND IS THE ONE FACT THIS CRASH COULD NOT STATE. SIX call
           sites reach this wrapper — five in this file and the host's record_cursor_run — so "its call site"
           named an action with no object, and the remedy it prescribed (a skipped coercion) is only ONE of the
           two shapes that land here: the other is an operand a CHAIN WALK produced, which no caller could have
           coerced. Nothing told those two apart, and they take opposite work. The tag does, for free.
           Measured: a `for-in` over a concolic record aborts here while `Object.keys` and
           `Object.getOwnPropertyNames` over the SAME record complete and fork normally — so the operand is one
           the enumeration seam never saw. The CALLER is already recoverable from the abort's frame list, which
           is why the site is not threaded here as well.
           THE TAG IS FIRST BECAUSE A CUT STILL COSTS THE TAIL, WHICH IS A WEAKER REASON THAN THE ONE THAT
           STOOD HERE. This said the buffer "cuts silently" and that the payload sat at the front where a
           lengthening edit could not take it — a workaround for a missing primitive, and the primitive is
           built: quickjs-check.h composes with the same bare cap, then reads snprintf's own return and LABELS
           an overlong reason, so a future edit that outgrows 512 now says how much of itself is missing
           instead of dropping its end in silence. What labelling does NOT do is give the tail back. So the
           ordering stays and its reason changes: the operand is the one fact this crash could not otherwise
           state, and it belongs where only a cut that ate the whole reason could reach it. */
        DCHECKF(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
                "[[OwnPropertyKeys]] over a NON-OBJECT operand, tag %d (quickjs.h JS_TAG_*: -7 STRING, 0 INT, "
                "2 NULL, 3 UNDEFINED, 4 UNINITIALIZED). §10.1.11 is an OBJECT internal method and this "
                "request's delivery reads the operand as one, so either a coercion owed by the asking "
                "algorithm's own spec step was skipped at its call site, or a CHAIN WALK handed on a link its "
                "own last request answered — no caller could have coerced the second, and the tag tells them "
                "apart",
                (int)JS_VALUE_GET_TAG(obj));
        /* THE NULL TEST IS THE HOST'S ANSWER AND NOT A FALLBACK: a host that installs no concolic value class
           at all (a conformance run) has no record to ask about, and there is no second implementation for this
           to select between — the hook itself answers JS_UNINITIALIZED for every object that carries no
           question, which is where the condition that decides that lives. */
        if (h->keys_probe == KEYS_PROBE_UNASKED) {
            if (JS_IsUninitialized(h->keys_pred))
                h->keys_pred = g_concolic.own_keys_pred(ctx, obj);
            if (!JS_IsUninitialized(h->keys_pred)) {
                r = step_tobool_run(ctx, h, h->keys_pred, OWNKEYS_PRED_OP, &arm);
                if (r) return r < 0 ? -1 : r;
                JS_FreeValue(ctx, h->keys_pred);
                h->keys_pred = JS_UNINITIALIZED;
                /* THE ARM ON WHICH THE RECORD HOLDS A MEMBER, PERFORMED — see step_ownkeys_chain, and
                   JSConcolicHooks.own_key_mint for why it is a materialisation rather than a key appended to
                   the answer. The chain runs to its end BEFORE the request is issued, so the walk that builds
                   the List sees every member this flow decided the record holds. */
                if (arm)
                    h->keys_probe = 0;
            }
        }
        if (h->keys_probe != KEYS_PROBE_UNASKED) {
            r = step_ownkeys_chain(ctx, h, obj);
            if (r) return r < 0 ? -1 : r;
            h->keys_probe = KEYS_PROBE_UNASKED;
        }
        step_keyed_inflight(h);
        h->cb_coerce[0] = (JSValue)obj;   /* borrowed: the machine holds the receiver across the request */
        *out_cb = h->cb_coerce; *out_argc = 0;
        h->keys_phase = GET_PH_GOT;
        return 11;
    }
    DCHECK(h->keys_phase == GET_PH_GOT, "an own-keys request was delivered with none in flight on this header");
    /* THE CHAIN'S WHOLE LIFE IS INSIDE ONE ENTRY INTO THE PHASE ABOVE, asserted HERE because here is the far
       side of the request — the driver, a park and a clone all stand between the two, and this is where a
       cursor that survived any of them shows up. One still open would have members to materialise after the
       walk that lists them has already run, so its remaining positions would be enumerated by nobody. */
    DCHECK(h->keys_probe == KEYS_PROBE_UNASKED,
           "an unknown-member chain was still open when its enumeration's key list came back");
    step_keyed_answered(h);   /* it carries no key either, so the stage is its whole site identity */
    h->keys_phase = GET_PH_START;
    if (JS_IsException(in))
        return -1;
    *pout = in;
    return 0;
}

/* GetOption(options, key) for an ENUMERATED STRING option, over step_getprop_run. The read is an ordinary
   [[Get]] on a bag the PAGE supplies, so it is an accessor or a Proxy trap away from being the page's code — the
   base64 family performed all five of theirs with JS_GetPropertyStr from C. `allowed` is a NULL-terminated list
   and the answer is its INDEX; `fallback` is what undefined means. Two-phase like every sub-sequence: it parks on
   the read and answers at the SAME call site.
     0 = *pout is set, 6 = the caller must return that step code, -1 = threw. */
static int step_strop_run(JSContext *ctx, JSStepHdr *h, JSValueConst options, JSAtom key,
                          const char *const *allowed, int fallback, JSValue in, int *pout,
                          JSValue **out_cb, int *out_argc)
{
    JSValue v;
    const char *str;
    int i, r;

    if (h->get_phase == GET_PH_START) {
        if (JS_IsUndefined(options)) { JS_FreeValue(ctx, in); *pout = fallback; return 0; }
        return step_getprop_run(ctx, h, options, key, in, &v, out_cb, out_argc);
    }
    r = step_getprop_run(ctx, h, options, key, in, &v, out_cb, out_argc);
    if (r < 0) return -1;   /* the getter threw; GetOption has no catch of its own */
    DCHECK(r == 0, "an option read answered with something other than its value");
    if (JS_IsUndefined(v)) { *pout = fallback; return 0; }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "option value must be a string");
        return -1;
    }
    str = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!str) return -1;
    for (i = 0; allowed[i]; i++) {
        if (!strcmp(str, allowed[i])) { JS_FreeCString(ctx, str); *pout = i; return 0; }
    }
    JS_FreeCString(ctx, str);
    JS_ThrowTypeError(ctx, "invalid option value");
    return -1;
}

/* 27.5.4.7.1 PromiseResolve ( ctor, resolution ) with ctor = %Promise%, as a SUB-SEQUENCE. TWO of its steps are
   the page's code, and both are reachable through the one condition "resolution is an Object": step 1.a's
   `constructor` READ on a resolution that is already a promise (an accessor or a Proxy supplies it —
   return-suspendedStart-broken-promise.js defines exactly that), and step 3's RESOLVE, whose resolving function
   reads `then` off a thenable (27.5.1.3 step 2.f). js_promise_resolve_native performed the first from C and
   handed the second to the resolving function's C entry; it now REFUSES an Object resolution outright, so this
   is the only implementation of either and there is no route left to choose between.
   `pout` is written EXACTLY ONCE, on the return-0 exit, like every other sub-sequence: the promise exists before
   the resolve request parks, so holding it in the caller's local would lose it across the suspension — it lives
   in cb[3] instead, which the caller already owns for the duration. `cb` is a 4-slot buffer the CALLER lends and
   OWNS; its fini must free it, because the resolve request can park and the machine can be abandoned inside it. */
enum { PRR_START = 0, PRR_CTOR, PRR_RESOLVE };
static int step_promiseresolve_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, JSValueConst x,
                                   JSValue *pout, JSValue *cb, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValue ctorv = JS_UNDEFINED, funcs[2];
    int r;

    if (*phase == PRR_START) {
        if (!JS_GetOpaque(x, JS_CLASS_PROMISE))
            goto build;                    /* step 1 is skipped: not a promise, so no `constructor` read */
        *phase = PRR_CTOR;
        r = step_getprop_run(ctx, h, x, JS_ATOM_constructor, in, &ctorv, out_cb, out_argc);
        in = JS_UNDEFINED;                 /* consumed either way */
        if (r) return r;
        goto have_ctor;
    }
    if (*phase == PRR_CTOR) {
        r = step_getprop_run(ctx, h, x, JS_ATOM_constructor, in, &ctorv, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r) return r;
    have_ctor:
        {
            bool same = js_same_value(ctx, ctorv, ctx->promise_ctor);
            JS_FreeValue(ctx, ctorv);
            if (same) { *phase = PRR_START; *pout = js_dup(x); return 0; }   /* step 1.b: x IS the answer */
        }
    build:
        JS_FreeValue(ctx, in);
        /* NewPromiseCapability(%Promise%): the native one, so there is no Construct and nothing observable. */
        cb[3] = js_new_promise_capability(ctx, funcs, JS_UNDEFINED);
        if (JS_IsException(cb[3])) { cb[3] = JS_UNDEFINED; return -1; }
        cb[0] = JS_UNDEFINED;
        cb[1] = funcs[0];
        cb[2] = js_dup(x);
        JS_FreeValue(ctx, funcs[1]);       /* step 3 calls resolve only; reject is never reachable from here */
        *phase = PRR_RESOLVE;
        *out_cb = cb; *out_argc = 1;
        return 3;
    }
    DCHECK(*phase == PRR_RESOLVE, "PromiseResolve resumed in an unknown phase");
    /* AN ABRUPT DELIVERY HERE WOULD BE SWALLOWED BY THE LINE BELOW, so it is asserted rather than tolerated: a
       machine that declares catches_abrupt is re-entered with JS_EXCEPTION for ANY failure of its request, and
       this answering path takes cb[3] as the answer whatever arrived. Step 3's Call is on the capability's own
       resolving function, which is the NATIVE one (step 2 built it without a Construct) and settles rather than
       throwing — so an exception reaching here is the DISPATCH having failed, and taking a promise over a live
       throw is how that becomes invisible. */
    DCHECK(!JS_IsException(in),
           "PromiseResolve's step 3 Call completed ABRUPTLY — its callee is the native capability's resolving "
           "function, which settles and never throws, so this delivery came from a failed dispatch and the "
           "answer below would hand out a promise over a live exception");
    JS_FreeValue(ctx, cb[1]); cb[1] = JS_UNDEFINED;
    JS_FreeValue(ctx, cb[2]); cb[2] = JS_UNDEFINED;
    JS_FreeValue(ctx, in);                 /* resolve's result is discarded */
    *pout = cb[3]; cb[3] = JS_UNDEFINED;
    DCHECK(JS_GetOpaque(*pout, JS_CLASS_PROMISE) != NULL,
           "PromiseResolve answered with something that is not a promise");
    *phase = PRR_START;
    return 0;
}

/* A keyed WRITE, Set(O, key, value, true) — the mirror of the read above and the half that was missing. Every
   builtin that copies properties (Object.assign's target, Array.from's final `length`) performed it with
   JS_SetProperty straight out of C, so a SETTER or a Proxy `set` trap with a loop in it had no flow base. WHICH
   shape it turns out to be — a data slot, a bytecode setter, a trap — is the driver's decision, made in the one
   place the read's already is. The value is BORROWED: the continuation dups what it needs for the invariant.
     0 = done, 8 = the caller must return that step code, -1 = threw. */
static int step_setprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValueConst value,
                            JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JS_FreeValue(ctx, in);
        DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed operation is already in flight on this machine's header");
        step_keyed_inflight(h);
        h->get_atom = JS_DupAtom(ctx, atom);
        h->cb_coerce[0] = (JSValue)obj;     /* borrowed: the machine holds both across the write */
        h->cb_coerce[1] = (JSValue)value;
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        h->get_phase = GET_PH_GOT;
        return 8;
    }
    DCHECK(atom == JS_ATOM_NULL || atom == h->get_atom,
           "a keyed write was answered at a DIFFERENT call site than the one that parked it — see the read's "
           "contract above");
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing setter or `set` trap */
    JS_FreeValue(ctx, in);                  /* a write delivers no value */
    return 0;
}

/* THE SAME WRITE WITH ECMAScript §7.3.4 Set ( obj, propertyKey, value, throw )'s FLAG FALSE — the receiver's
 * own [[Set]], whose boolean is DISCARDED rather than turned into a TypeError, so it is request 14 and not
 * the throwing form above. The receiver is the OBJECT, which is what §7.3.4 states ("Let success be ?
 * obj.[[Set]](propertyKey, value, obj)"); only the answer differs. On a Proxy it is the `set` trap, and on an
 * accessor it is the setter, exactly as request 8 is.
 * THE PARAMETER NAMES ARE THE STANDARD'S CURRENT ONES. This paragraph cited the heading as `Set ( O, P, V,
 * Throw )` and quoted `Let success be ? O.[[Set]](P, V, O)`, and ECMAScript renamed those parameters — so the
 * quotation named a sentence that occurs nowhere in the specification while the NUMBER stayed right, which is
 * the one shape a section-number checker cannot see.
 *
 * IT IS EXPORTED AND ITS THROWING TWIN IS NOT, because the caller that needed a keyed write from OUTSIDE the
 * engine needed THIS one: Web IDL §3.7.6 Attributes' create-an-attribute-setter step 4.5.8.4 is "Perform
 * ? Set(Q, forwardId, V, false)" — the [PutForwards] forwarding, whose whole point is that a target refusing
 * the write is not an error the forwarding invents. Handing that caller the throwing form would have been the
 * near-miss that reads correct: it agrees for every Q whose forwarded member is an accessor with a setter, and
 * it manufactures a TypeError the moment a page redefines the attribute this forwards THROUGH — which is a
 * shape reachable from script, since not every [PutForwards] carrier is [LegacyUnforgeable].
 *   0 = done, 14 = the caller must return that step code, -1 = threw. */
int step_setprop_bare_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValueConst value,
                          JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JS_FreeValue(ctx, in);
        DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed operation is already in flight on this machine's header");
        step_keyed_inflight(h);
        h->get_atom = JS_DupAtom(ctx, atom);
        h->cb_coerce[0] = (JSValue)obj;     /* borrowed: the machine holds all three across the write */
        h->cb_coerce[1] = (JSValue)obj;     /* 7.3.4's RECEIVER is O itself, never a third operand */
        h->cb_coerce[2] = (JSValue)value;
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        h->get_phase = GET_PH_GOT;
        return 14;
    }
    DCHECK(atom == JS_ATOM_NULL || atom == h->get_atom,
           "a bare keyed write was answered at a DIFFERENT call site than the one that parked it — see the "
           "read's contract above");
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing setter or `set` trap */
    JS_FreeValue(ctx, in);                  /* the boolean [[Set]] answered is Set(O,P,V,false)'s to discard */
    return 0;
}

/* an INDEX key. ! ToString(𝔽(idx)) is what the spec's element access reduces to, and the atom is the engine's
   representation of exactly that. */
static int step_getidx_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, int64_t idx, JSValue in,
                           JSValue *pout, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JSAtom atom;
        JS_FreeValue(ctx, in);
        atom = JS_NewAtomInt64(ctx, idx);
        if (atom == JS_ATOM_NULL) return -1;
        return step_getprop_begin(ctx, h, obj, atom, out_cb, out_argc);
    }
    /* the INDEX form has no key to check on the answering path: recomputing the atom to satisfy a DCHECK would
       allocate on the hot path, so it passes NULL and the "one is in flight" half of the contract still holds. */
    return step_getprop_done(ctx, h, JS_ATOM_NULL, in, pout);
}

/* Set(O, ! ToString(𝔽(idx)), v, true) — the write half of an element access. A setter or a Proxy `set` trap is
   the page's code exactly as `get` is, and every builtin that fills a receiver reached it with
   JS_SetPropertyInt64 from C. 0 = done, 8 = the caller must return that step code, -1 = threw. */
static int step_setidx_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, int64_t idx, JSValueConst value,
                           JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JSAtom atom;
        int r;
        atom = JS_NewAtomInt64(ctx, idx);
        if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, in); return -1; }
        r = step_setprop_run(ctx, h, obj, atom, value, in, out_cb, out_argc);
        JS_FreeAtom(ctx, atom);   /* step_setprop_run took its own reference */
        return r;
    }
    return step_setprop_run(ctx, h, obj, JS_ATOM_NULL, value, in, out_cb, out_argc);
}

/* DeletePropertyOrThrow(O, ! ToString(𝔽(idx))): a Proxy `deleteProperty` trap is the page's code, and
   JS_DeletePropertyInt64 reached it from C. 0 = done, 9 = the caller must return that step code, -1 = threw. */
static int step_delidx_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, int64_t idx,
                           JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JSAtom atom;
        JS_FreeValue(ctx, in);
        atom = JS_NewAtomInt64(ctx, idx);
        if (atom == JS_ATOM_NULL) return -1;
        DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed operation is already in flight on this machine's header");
        step_keyed_inflight(h);
        h->get_atom = atom;                 /* the conversion already owns it */
        h->cb_coerce[0] = (JSValue)obj;     /* borrowed: the machine holds it across the delete */
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        h->get_phase = GET_PH_GOT;
        return 9;
    }
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing `deleteProperty` trap */
    JS_FreeValue(ctx, in);                  /* a delete delivers no value */
    return 0;
}

/* CreateDataPropertyOrThrow(O, ! ToString(𝔽(idx)), v) — how every builtin FILLS a result object. On a Proxy it
   is the `defineProperty` trap, which is the page's code, and JS_DefinePropertyValueInt64 reached it from C.
   0 = done, 10 = the caller must return that step code, -1 = threw. */
static int step_defidx_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, int64_t idx, JSValueConst value,
                           JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JSAtom atom;
        JS_FreeValue(ctx, in);
        atom = JS_NewAtomInt64(ctx, idx);
        if (atom == JS_ATOM_NULL) return -1;
        DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed operation is already in flight on this machine's header");
        step_keyed_inflight(h);
        h->get_atom = atom;                 /* the conversion already owns it */
        h->cb_coerce[0] = (JSValue)obj;     /* borrowed: the machine holds both across the define */
        h->cb_coerce[1] = (JSValue)value;
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        h->get_phase = GET_PH_GOT;
        return 10;
    }
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing `defineProperty` trap */
    JS_FreeValue(ctx, in);                  /* a define delivers no value */
    return 0;
}

/* CreateDataProperty(O, key, v) for a NAMED key — the sibling of step_setprop_run, and the same request
   step_defidx_run issues for an index. On a Proxy it is the `defineProperty` trap.
   `or_throw` IS the spec's own distinction, not a convenience: CreateDataPropertyOrThrow raises a TypeError when
   [[DefineOwnProperty]] answers false, and CreateDataProperty (§25.5.2.4's, InternalizeJSONProperty) YIELDS that
   false and carries on. A trap returning false is what tells the two apart, so the caller names which one the
   step it is implementing says.
     0 = done, 10 = the caller must return that step code, -1 = threw.
   A TRAP THAT THREW IS NOT A TRAP THAT ANSWERED FALSE, and this used to answer as if it were: the answering
   path rewound the cursor by hand and returned 0 whatever arrived, so a `defineProperty` trap's throw was
   delivered as a successful define. Its own INDEX twin two functions up reports the same event as -1 through
   step_keyed_abrupt, and both of this one's callers were already written for that (`if (r < 0) return -1`) —
   one request answering differently depending on whether its key was a name or an index, with the caller's
   handling for the honest answer already in place and unreachable.
   THE SPEC SAYS THE THROW PROPAGATES, with a `?`: §7.3.37 SetterThatIgnoresPrototypeProperties ( thisValue,
   home, propertyKey, value ) step 4.a is "Perform ? CreateDataPropertyOrThrow(thisValue, propertyKey, value)",
   so a `defineProperty` trap that threw on the receiver is that setter's completion. It COMPLETED NORMALLY
   instead, with the throw still live in the context, and nothing downstream reports that: step_request_check
   fires only on a machine that goes on to ASK for something, and this one returns DONE. */
static int step_defprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValueConst value,
                            bool or_throw, JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JS_FreeValue(ctx, in);
        DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed operation is already in flight on this machine's header");
        step_keyed_inflight(h);
        h->get_atom = JS_DupAtom(ctx, atom);
        h->cb_coerce[0] = (JSValue)obj;     /* borrowed: the machine holds both across the define */
        h->cb_coerce[1] = (JSValue)value;
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        if (!or_throw) {
            /* the descriptor is CreateDataProperty's own; only the THROW bit differs, and the relay reads
               gp_no_throw off its absence. */
            DCHECK(h->desc_flags == 0, "a descriptor shape is already in flight on this machine's header");
            h->desc_flags = JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE
                          | JS_PROP_HAS_CONFIGURABLE | JS_PROP_C_W_E;
        }
        h->get_phase = GET_PH_GOT;
        return 10;
    }
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing `defineProperty` trap */
    JS_FreeValue(ctx, in);                  /* a define delivers no value */
    return 0;
}

/* The BARE `O.[[Delete]](key)` for a NAMED key — §25.5.2.4's, which yields its boolean rather than throwing on
   false, so it is request 15 and not the DeletePropertyOrThrow step_delidx_run issues. On a Proxy it is the
   `deleteProperty` trap.
     0 = done, 15 = the caller must return that step code. */
static int step_delprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom,
                            JSValue in, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JS_FreeValue(ctx, in);
        DCHECK(h->get_atom == JS_ATOM_NULL, "a keyed operation is already in flight on this machine's header");
        step_keyed_inflight(h);
        h->get_atom = JS_DupAtom(ctx, atom);
        h->cb_coerce[0] = (JSValue)obj;     /* borrowed: the machine holds it across the delete */
        *out_cb = h->cb_coerce; *out_argc = (int)atom;
        h->get_phase = GET_PH_GOT;
        return 15;
    }
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* a throwing `deleteProperty` trap */
    JS_FreeValue(ctx, in);                  /* the boolean [[Delete]] answered is §25.5.2.4's to discard */
    return 0;
}

/* HasProperty(O, key) for a NAMED key, borrowed from the caller — InstallErrorCause asks it of `options`, which
   can be a Proxy whose `has` trap is the page's code. Same request as the read, asking the other question. */
static int step_hasprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                            int *pres, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        int r;
        JS_FreeValue(ctx, in);
        r = step_getprop_begin(ctx, h, obj, JS_DupAtom(ctx, atom), out_cb, out_argc);
        return (r == 6) ? 7 : r;
    }
    /* the END comes first, and so does the abrupt test: JS_ToBool of the JS_EXCEPTION marker would answer
       "present" for a `has` trap that threw, which is a membership answer invented out of a throw. */
    if (step_keyed_abrupt(ctx, h, in)) return -1;
    *pres = JS_ToBool(ctx, in);
    JS_FreeValue(ctx, in);
    return 0;
}

int step_hasproperty_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in, int *pres,
                         JSValue **out_cb, int *out_argc)
{
    return step_hasprop_run(ctx, h, obj, atom, in, pres, out_cb, out_argc);
}

/* HasProperty(O, ! ToString(𝔽(idx))) — the OTHER half of the element access indexOf/lastIndexOf perform, which
   skip holes and so must ask before they read. A Proxy `has` trap is the page's code exactly as `get` is, and
   JS_TryGetPropertyInt64 reached it from C. 0 = *pres is filled, 7 = the caller must return that step code. */
static int step_hasidx_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, int64_t idx, JSValue in,
                           int *pres, JSValue **out_cb, int *out_argc)
{
    if (h->get_phase == GET_PH_START) {
        JSAtom atom;
        int r;
        JS_FreeValue(ctx, in);
        atom = JS_NewAtomInt64(ctx, idx);
        if (atom == JS_ATOM_NULL) return -1;
        r = step_getprop_begin(ctx, h, obj, atom, out_cb, out_argc);
        return (r == 6) ? 7 : r;   /* the same request, asking the other question */
    }
    if (step_keyed_abrupt(ctx, h, in)) return -1;   /* see the named form: a thrown `has` is not a "present" */
    *pres = JS_ToBool(ctx, in);
    JS_FreeValue(ctx, in);
    return 0;
}

