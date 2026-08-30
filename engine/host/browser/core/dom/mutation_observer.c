/* DOM §4.3 "Mutation observers".
 *
 * WHY THIS MATTERS TO A SOLVER. A bundle that renders through a framework does not call your code when the DOM
 * changes — it observes. `new MutationObserver(cb).observe(root, {childList:true, subtree:true})` is how a
 * router, a lazy-image loader, an analytics shim and every "hydrate what just appeared" path is wired, and the
 * fetches behind those live INSIDE the callback. With the interface absent the page's own `new
 * MutationObserver(...)` throws before any of it runs; with the interface present but the records never
 * delivered, the callback is registered and silently never called, which is worse — nothing throws and the
 * whole branch is simply unreachable.
 *
 * THREE THINGS ARE PER-FLOW STATE AND THEREFORE JS VALUES, not malloc'd C:
 *   - a node's REGISTERED OBSERVER LIST (an own slot on its wrapper),
 *   - an observer's NODE LIST and RECORD QUEUE (own slots on the observer),
 *   - the agent's PENDING MUTATION OBSERVERS (one Array this file holds).
 * The DOM is a time-travel entity: two forked arms see different trees, so they must see different record
 * queues, and an arm that appends a record must not be observable from its sibling. An Array gives both for
 * free — its mutations are property writes the COW delta already captures, and the snapshot machinery already
 * carries it — while a C list captured by its head POINTER reverts the pointer on a context switch and leaves
 * the records reachable from nothing (CLAUDE.md, §State isolation).
 *
 * THE MICROTASK IS SCHEDULER WORK, NOT A DRAIN. §4.3's "queue a mutation observer microtask" enqueues an
 * ordinary job, so notify-mutation-observers is a first-class flow in the one WFQ: preemptible, parkable, and
 * resumable from the exact reaction it was inside. It has to be — step 6.4 INVOKES THE PAGE'S CALLBACK, which
 * is arbitrary code with loops and awaits in it, so the notify machine parks there like any other member.
 *
 * TRANSIENT REGISTERED OBSERVERS ARE THE PART THAT IS OMITTED SILENTLY. `subtree: true` on a parent must keep
 * reporting a REMOVED child's own mutations until the observer is next notified, and §4.2.3's remove step 15
 * is what makes that true: it copies every subtree registration of every inclusive ancestor onto the node
 * being removed, marked with the registration it came from. Leave it out and nothing throws, nothing asserts,
 * and a page that detaches a node and then fills it in hears about none of it.
 *
 * WHAT IS HONESTLY ABSENT: §4.3's SIGNAL SLOTS (notify steps 4, 5 and 7). They are the shadow-DOM half — a
 * `slotchange` event fired from this same microtask — and this engine has no shadow root, no slot and no
 * "signal a slot change" for anything to append to. An always-empty set walked by an always-empty loop is the
 * shape §NO STUBS names; the concept arrives with `attachShadow`. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/dom/mutation_record.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/slot.h"
#include "core/events/report_exception.h"

/* ---- the shapes ------------------------------------------------------------------------------------------
 *
 * A MutationObserver's own state is one Array in an own slot: [callback, node list, record queue]. A node's
 * registered observer list is one Array in an own slot on its WRAPPER, whose entries are themselves Arrays:
 * [observer, options, source]. §4.3 defines a transient registered observer as "a registered observer that
 * ALSO consists of a source", so `source` present IS the whole difference and there is no second entry kind to
 * keep in step with the first. */
enum { MO_S_CALLBACK = 0, MO_S_NODES, MO_S_RECORDS, MO_S_COUNT };
enum { RO_OBSERVER = 0, RO_OPTIONS, RO_SOURCE, RO_COUNT };

static JSClassID g_mo_class;
static JSValue   g_mo_key = JS_UNDEFINED;      /* the observer's own state slot */
static JSAtom    g_atom_mo = JS_ATOM_NULL;
static JSValue   g_ro_key = JS_UNDEFINED;      /* a node's registered observer list slot */
static JSAtom    g_atom_ro = JS_ATOM_NULL;
static JSAtom    g_atom_queued = JS_ATOM_NULL; /* §4.3's "mutation observer microtask queued" */
static JSValue   g_pending = JS_UNDEFINED;     /* §4.3's "pending mutation observers", the agent's */
/* THE MICROTASK'S CALLEE IS MINTED PER REALM. js_call_c_function does `ctx = p->u.cfunc.realm`, so a driver
   held in one static would run every document's notification — and fire every document's `slotchange` — out of
   whichever realm happened to build it first (§3.7). The step DEFINITION is the agent's; the function OBJECT
   is the realm's, in core/realm.h's per-realm value store. */
static int       g_notify_stepid = -1, g_notify_slot = -1;
static int       g_id_observe = -1, g_id_disconnect = -1, g_id_take = -1, g_id_ctor = -1;
static bool      g_any_observer;
static int       g_ready;

bool mutation_observer_any(void) { return g_any_observer; }

static uint32_t mo_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, (JSValue)arr, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void mo_push(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, mo_len(ctx, arr), v);
}

static void mo_set_len(JSContext *ctx, JSValueConst arr, uint32_t n)
{
    JS_SetPropertyStr(ctx, (JSValue)arr, "length", JS_NewUint32(ctx, n));
}

/* "a CLONE of" — §4.3 says it of the record queue, of the pending set and of takeRecords' answer, and it means
   a new list holding the same members. A fresh Array, because the original is then EMPTIED and a reference to
   it would empty the copy too. */
static JSValue mo_clone(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = mo_len(ctx, arr), i;
    JSValue out = JS_NewArray(ctx);

    CHECK(!JS_IsException(out), "a §4.3 list clone could not be allocated");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, (JSValue)arr, i));
    return out;
}

/* THE OBSERVER'S OWN STATE. OWNED. */
static JSValue mo_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_mo_class)
        return JS_ThrowTypeError(ctx, "not a MutationObserver");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom_mo) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s), "a MutationObserver carries no state — the constructor writes all three fields "
                           "before the object exists, so one without them was made somewhere else");
    return s;
}

/* A NODE'S REGISTERED OBSERVER LIST, off its wrapper. `create` mints an empty one; without it an unobserved
   node stays a node with no extra slot, which is what keeps the walk in §4.3.2 free for a document nobody is
   watching. OWNED, JS_UNDEFINED when there is none. */
static JSValue mo_ro_list(JSContext *ctx, JSValueConst wrap, int create)
{
    JSValue l;

    if (JS_GetOwnSlot(ctx, &l, wrap, g_atom_ro) > 0) {
        if (JS_IsObject(l)) return l;
        JS_FreeValue(ctx, l);
    }
    if (!create) return JS_UNDEFINED;
    l = JS_NewArray(ctx);
    CHECK(!JS_IsException(l), "a node's registered observer list could not be allocated");
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_ro, JS_DupValue(ctx, l), 0);
    return l;
}

/* A MutationObserverInit member, as §4.3.1's steps ask about it: does it EXIST, and is it TRUE. The two are
   different questions and every one of observe()'s six validation steps turns on which one it is asking —
   `observe(t, {attributes:false, attributeFilter:[]})` is a TypeError while `observe(t, {attributeFilter:[]})`
   is not, and a conversion that folded absence into false could not tell them apart. */
static JSValue mo_opt(JSContext *ctx, JSValueConst options, const char *name)
{
    return JS_GetPropertyStr(ctx, (JSValue)options, name);
}

static bool mo_opt_exists(JSContext *ctx, JSValueConst options, const char *name)
{
    JSValue v = mo_opt(ctx, options, name);
    bool e = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    return e;
}

static bool mo_opt_true(JSContext *ctx, JSValueConst options, const char *name)
{
    JSValue v = mo_opt(ctx, options, name);
    bool t = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return t;
}

/* ---- §4.3 QUEUE A MUTATION OBSERVER MICROTASK / NOTIFY MUTATION OBSERVERS -------------------------------- */

/* §4.3: "If the surrounding agent's mutation observer microtask queued is true, then return. Set it to true.
   Queue a microtask to notify mutation observers." The FLAG is what makes many mutations in one turn deliver
   ONE callback invocation carrying every record, which is the observable behaviour the whole design is for. */
void mutation_observer_queue_microtask(JSContext *ctx)
{
    JSValue f, fn;
    int set;

    DCHECK(g_ready, "a mutation observer microtask was queued before mutation_observer_init ran");
    f = JS_GetProperty(ctx, g_pending, g_atom_queued);
    set = JS_ToBool(ctx, f);
    JS_FreeValue(ctx, f);
    if (set) return;
    JS_SetProperty(ctx, g_pending, g_atom_queued, JS_TRUE);
    fn = realm_value_get(ctx, g_notify_slot);
    DCHECK(JS_IsFunction(ctx, fn),
           "a mutation observer microtask was queued in a realm that never built its notify driver");
    JS_EnqueueCallJob(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

/* §4.3 notify step 6.3 — "remove all transient registered observers whose observer is mo from node's
   registered observer list", for one node. */
static void mo_drop_transients(JSContext *ctx, JSValueConst wrap, JSValueConst observer)
{
    JSValue list = mo_ro_list(ctx, wrap, 0);
    uint32_t n, i, out = 0;

    if (!JS_IsObject(list)) { JS_FreeValue(ctx, list); return; }
    n = mo_len(ctx, list);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        JSValue src = JS_GetPropertyUint32(ctx, e, RO_SOURCE);
        JSValue ob = JS_GetPropertyUint32(ctx, e, RO_OBSERVER);
        bool drop = !JS_IsUndefined(src) && JS_VALUE_GET_PTR(ob) == JS_VALUE_GET_PTR(observer);

        JS_FreeValue(ctx, src);
        JS_FreeValue(ctx, ob);
        if (drop) { JS_FreeValue(ctx, e); continue; }
        JS_SetPropertyUint32(ctx, list, out++, e);
    }
    if (out != n) mo_set_len(ctx, list, out);
    JS_FreeValue(ctx, list);
}

#define MO_NOTIFY_STAGES(X) \
    X(MO_NOTIFY_CALLBACK, "DOM §4.3 notify mutation observers step 6.4 (invoking mo's callback with " \
                          "« records, mo » and \"report\", with callback this value mo)") \
    X(MO_NOTIFY_REPORT,   "DOM §4.3 notify mutation observers step 6.4's \"report\", which is HTML §8.1.4.6 " \
                          "report an exception (it fires an `error` event at the global)") \
    X(MO_NOTIFY_SLOTCHANGE, "DOM §4.3 notify mutation observers step 7 (firing `slotchange`, bubbles true, at " \
                            "each slot of signalSet — one slot per resume, because each fire runs the page's " \
                            "listeners)")
enum { MO_NOTIFY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const MO_NOTIFY_STEPS[] = { MO_NOTIFY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSMoNotify {
    JSStepHdr hdr;        /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   notify;     /* step 2's notifySet (the clone), UNDEFINED before step 1 has run */
    uint32_t  i;          /* the cursor into it */
    JSValue   cur;        /* the mo whose callback is in flight (owned) */
    JSValue   records;    /* step 6.1's clone, held across the park because it IS the argument */
    uint8_t   phase;      /* step_call_run's own */
    /* HAVE THE OWNED FIELDS BEEN PLACED YET. It is its own byte and not a JS_IsUndefined test on `notify`,
       because tramp_step_state_new js_mallocz's the state and a ZEROED JSValue is the INTEGER 0 — JS_TAG_INT
       is 0 — so every value on a fresh state reads as "already set". Reading "have I begun" off one is always
       "yes", which skipped steps 1-3 entirely and then handed step 6.4 the integer 0 as the observer. */
    uint8_t   started;
    uint8_t   reporting;
    JSValue   exc;
    ReportExceptionWork rep;
    /* §4.3 steps 4-5 and 7 — the signal-slots half of THIS notification, held here because the fire parks and
       the algorithm is one. slot.c owns what it means. */
    SlotChangeWork      slots;
    JSValue   cb[4];      /* [this, callback, records, mo] */
} JSMoNotify;

static void js_mo_notify_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSMoNotify *s = st;
    int k;

    v->val(ctx, &s->notify);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->records);
    v->val(ctx, &s->exc);
    report_exception_work_visit(ctx, &s->rep, v);
    slot_change_work_visit(ctx, &s->slots, v);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static JSValue js_mo_notify_fini(JSContext *ctx, void *st, bool take_result)
{
    JSMoNotify *s = st;

    (void)take_result;
    /* §8.1.4.6 step 5's FLAG, if a report was abandoned holding it. It is not a reference, so no declaration
       names it and the discharge cannot give it back; the record's REFERENCES are named by js_mo_notify_visit
       and released through it, which is why this is the unlock and not the whole release. */
    report_exception_work_unlock(ctx, &s->rep);
    return JS_UNDEFINED;
}

static int js_mo_notify_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSMoNotify *s = st;
    int r, k;

    DCHECK(s->hdr.stage >= MO_NOTIFY_CALLBACK && s->hdr.stage <= MO_NOTIFY_SLOTCHANGE,
           "notify mutation observers resumed into a stage §4.3 does not have");
    if (!s->started) {
        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL, because the failure path tears the
           state down through `fini`, which frees exactly what the state holds — and on a js_mallocz'd state
           that is nine integer zeros until this line runs. */
        s->notify = s->cur = s->records = s->exc = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        report_exception_work_start(&s->rep);
        slot_change_work_start(&s->slots);
        s->started = 1;
        /* STEPS 1-3, ON FIRST ENTRY. The flag is cleared BEFORE the walk, so a mutation performed by one of
           these callbacks schedules a NEW microtask rather than joining the batch being delivered — which is
           what makes `observer.observe(...); mutate(); ` inside a callback deliver a second time.
           The ARRAY is not replaced: it is the agent's, held in a C static no COW delta captures, so swapping
           the static would make one flow's replacement visible to every other. It is CLONED and then
           TRUNCATED, and the truncation is a property write the delta does capture. */
        JS_SetProperty(ctx, g_pending, g_atom_queued, JS_FALSE);      /* step 1 */
        s->notify = mo_clone(ctx, g_pending);                          /* step 2 */
        mo_set_len(ctx, g_pending, 0);                                 /* step 3 */
        slot_signal_slots_take(ctx, &s->slots);                        /* steps 4-5 */
        s->i = 0;
    }
    /* STEP 7 IS RESUMED FIRST, because once the observer walk is over the machine only ever parks inside a
       `slotchange` listener and a re-entry must land back on the slot whose fire is in flight. */
    if (s->hdr.stage == MO_NOTIFY_SLOTCHANGE) {
        r = slot_change_work_run(ctx, &s->slots, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        return JS_STEP_DONE;
    }
    for (;;) {
        JSValue ignored = JS_UNDEFINED, cbfn;

        if (s->reporting) {
            r = report_exception_run(ctx, &s->rep, s->exc, cb_result, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            s->reporting = 0;
            /* BACK TO STEP 6.4, AND THE TRANSITION IS HERE RATHER THAN AT THE PARK. The stage used to be
               assigned from inside the `r > 0` arms of both sub-sequences — after the request had parked, with
               its cursor already at 1 — which is the shape STEP_GOTO refuses. It was harmless only because this
               machine re-derives its position from `reporting` and `cur` on the way back in, so the resume
               reached the call site it left; a machine that dispatched on the stage would have collected the
               other sub-sequence's answer. Each transition is now taken where the two sub-sequences are both at
               rest: entering a report is where the throw is caught, and leaving one is here. */
            STEP_GOTO(s->hdr.stage, MO_NOTIFY_CALLBACK, &s->phase, NULL);
            JS_FreeValue(ctx, s->exc);
            s->exc = JS_UNDEFINED;
        }
        if (JS_IsUndefined(s->cur)) {
            /* STEP 6: the next mo whose records are worth delivering. Steps 6.1-6.3 run for EVERY mo in the
               set — the record queue is emptied and the transients are dropped even when there is nothing to
               deliver — and only step 6.4 is conditional on the records being non-empty. */
            for (;;) {
                JSValue mo, mstate, nodes;
                uint32_t nn, k;

                if (s->i >= mo_len(ctx, s->notify)) {          /* step 6 is over; step 7 remains */
                    JS_FreeValue(ctx, s->notify);
                    s->notify = JS_UNDEFINED;
                    s->i = 0;
                    STEP_GOTO(s->hdr.stage, MO_NOTIFY_SLOTCHANGE, &s->phase, NULL);
                    r = slot_change_work_run(ctx, &s->slots, cb_result, out_cb, out_argc);
                    if (r > 0) return r;
                    return JS_STEP_DONE;
                }
                mo = JS_GetPropertyUint32(ctx, s->notify, s->i++);
                mstate = mo_state(ctx, mo);
                DCHECK(!JS_IsException(mstate), "the agent's pending mutation observers holds something that "
                                                "is not a MutationObserver");
                s->records = JS_GetPropertyUint32(ctx, mstate, MO_S_RECORDS);   /* step 6.1 is the clone… */
                JS_SetPropertyUint32(ctx, mstate, MO_S_RECORDS, JS_NewArray(ctx));  /* …step 6.2 empties it */
                nodes = JS_GetPropertyUint32(ctx, mstate, MO_S_NODES);
                nn = mo_len(ctx, nodes);
                for (k = 0; k < nn; k++) {                                      /* step 6.3 */
                    JSValue w = JS_GetPropertyUint32(ctx, nodes, k);
                    mo_drop_transients(ctx, w, mo);
                    JS_FreeValue(ctx, w);
                }
                JS_FreeValue(ctx, nodes);
                JS_FreeValue(ctx, mstate);
                if (mo_len(ctx, s->records) == 0) {                             /* step 6.4's condition */
                    JS_FreeValue(ctx, s->records);
                    s->records = JS_UNDEFINED;
                    JS_FreeValue(ctx, mo);
                    continue;
                }
                s->cur = mo;
                break;
            }
        }
        /* STEP 6.4 — the page's callback, with `this` = mo and « records, mo ». */
        {
            JSValue mstate = mo_state(ctx, s->cur);
            JSValueConst args[2];

            DCHECK(!JS_IsException(mstate), "the notify walk lost its observer's state mid-callback");
            cbfn = JS_GetPropertyUint32(ctx, mstate, MO_S_CALLBACK);
            JS_FreeValue(ctx, mstate);
            args[0] = s->records;
            args[1] = s->cur;
            r = step_call_run(ctx, &s->phase, s->cb, 4, cbfn, s->cur, 2, args, cb_result, &ignored,
                              out_cb, out_argc);
            JS_FreeValue(ctx, cbfn);
            cb_result = JS_UNDEFINED;
            if (r > 0) return JS_STEP_CALL;
        }
        /* "and \"report\"": §4.3 invokes the callback with "report", so a callback that throws is reported and
           the walk goes on to the next observer. Without this one throwing callback would tear down the
           microtask and every observer behind it would silently never be notified. */
        if (JS_IsException(ignored)) {
            ignored = JS_UNDEFINED;
            s->exc = JS_GetException(ctx);
            s->reporting = 1;
            STEP_GOTO(s->hdr.stage, MO_NOTIFY_REPORT, &s->phase, NULL);
        }
        JS_FreeValue(ctx, ignored);                 /* MutationCallback returns undefined; anything else is discarded */
        JS_FreeValue(ctx, s->cur);
        JS_FreeValue(ctx, s->records);
        s->cur = s->records = JS_UNDEFINED;
    }
}

static const JSTrampStepDef js_mo_notify_def = {
    sizeof(JSMoNotify), js_mo_notify_step, js_mo_notify_fini, 0,
    /* Step 6.4 invokes with "report", so the callback's abrupt completion is this machine's own VALUE and must
       be delivered back to step() rather than tearing the microtask down. */
    .catches_abrupt = 1, .visit = js_mo_notify_visit,
    .algorithm = "DOM §4.3 notify mutation observers",
    .steps = MO_NOTIFY_STEPS
};

/* ---- §4.3.2 QUEUEING A MUTATION RECORD ------------------------------------------------------------------ */

/* The third bullet of the interested-observer test: "type is `attributes`, options["attributeFilter"] exists,
   and options["attributeFilter"] does not contain name OR namespace is non-null". Both halves reject, which is
   why a filtered observer never hears about an `xlink:href` however it spells the local name. */
static bool mo_filter_rejects(JSContext *ctx, JSValueConst options, const char *name, const char *ns)
{
    JSValue filter = mo_opt(ctx, options, "attributeFilter");
    uint32_t n, i;
    bool found = false;

    if (JS_IsUndefined(filter)) { JS_FreeValue(ctx, filter); return false; }
    if (ns != NULL) { JS_FreeValue(ctx, filter); return true; }
    n = mo_len(ctx, filter);
    for (i = 0; i < n && !found; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, filter, i);
        const char *s = JS_ToCString(ctx, v);

        if (s && name && !strcmp(s, name)) found = true;
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, filter);
    return !found;
}

void mutation_observer_queue_record(JSContext *ctx, int type, lxb_dom_node_t *target,
                                    const char *name, const char *ns, const char *old, size_t old_len,
                                    JSValueConst added, JSValueConst removed,
                                    lxb_dom_node_t *prev, lxb_dom_node_t *next)
{
    /* THE RECORD BELONGS TO THE TARGET'S DOCUMENT'S REALM, for the same reason §4.13.3's reaction does: the
       prototype a MutationRecord wears is that realm's, so building it in whichever realm happened to perform
       the mutation hands a page an object from another document. */
    JSContext *rctx = document_realm_of(target);
    JSValue obs, olds;
    lxb_dom_node_t *n;
    uint32_t k, count;

    DCHECK(target != NULL, "§4.3.2 was entered with no target");
    if (!g_ready || !g_any_observer) return;
    DCHECK(rctx != NULL, "a node in a document no realm was installed for was mutated — §4.3.2 builds the "
                         "record in that document's realm, so build its realm");

    obs = JS_NewArray(rctx);      /* step 1's interestedObservers, as its two columns */
    olds = JS_NewArray(rctx);
    CHECK(!JS_IsException(obs) && !JS_IsException(olds),
          "§4.3.2's interested-observer map could not be allocated");
    /* STEP 2/3: the inclusive ancestors of target, and each one's registered observer list. Walked upward,
       which IS "inclusive ancestors" in the order the spec lists them. */
    for (n = target; n; n = n->parent) {
        JSValue wrap = node_wrap(rctx, n);
        JSValue list = JS_IsNull(wrap) ? JS_UNDEFINED : mo_ro_list(rctx, wrap, 0);
        uint32_t ln, i;

        JS_FreeValue(rctx, wrap);
        if (!JS_IsObject(list)) { JS_FreeValue(rctx, list); continue; }
        ln = mo_len(rctx, list);
        for (i = 0; i < ln; i++) {
            JSValue e = JS_GetPropertyUint32(rctx, list, i);
            JSValue options = JS_GetPropertyUint32(rctx, e, RO_OPTIONS);
            JSValue mo = JS_GetPropertyUint32(rctx, e, RO_OBSERVER);
            bool skip;

            /* STEP 3.2's five bullets — "if NONE of the following are true", so each is a reason to SKIP. */
            skip = (n != target && !mo_opt_true(rctx, options, "subtree"))
                || (type == MR_TYPE_ATTRIBUTES && !mo_opt_true(rctx, options, "attributes"))
                || (type == MR_TYPE_ATTRIBUTES && mo_filter_rejects(rctx, options, name, ns))
                || (type == MR_TYPE_CHARACTER_DATA && !mo_opt_true(rctx, options, "characterData"))
                || (type == MR_TYPE_CHILD_LIST && !mo_opt_true(rctx, options, "childList"));
            if (!skip) {
                uint32_t on = mo_len(rctx, obs), j, at = on;

                for (j = 0; j < on; j++) {            /* step 3.2.2: the map keyed by the observer */
                    JSValue o = JS_GetPropertyUint32(rctx, obs, j);
                    bool same = JS_VALUE_GET_PTR(o) == JS_VALUE_GET_PTR(mo);
                    JS_FreeValue(rctx, o);
                    if (same) { at = j; break; }
                }
                if (at == on) {
                    JS_SetPropertyUint32(rctx, obs, at, JS_DupValue(rctx, mo));
                    JS_SetPropertyUint32(rctx, olds, at, JS_NULL);
                }
                /* STEP 3.2.3: the mapped old value, which is per OBSERVER and not per registration — one
                   registration asking for it is enough, and that is why the map holds a value rather than the
                   record being built here. */
                if ((type == MR_TYPE_ATTRIBUTES && mo_opt_true(rctx, options, "attributeOldValue")) ||
                    (type == MR_TYPE_CHARACTER_DATA && mo_opt_true(rctx, options, "characterDataOldValue")))
                    JS_SetPropertyUint32(rctx, olds, at,
                                         old ? JS_NewStringLen(rctx, old, old_len) : JS_NULL);
            }
            JS_FreeValue(rctx, options);
            JS_FreeValue(rctx, mo);
            JS_FreeValue(rctx, e);
        }
        JS_FreeValue(rctx, list);
    }

    count = mo_len(rctx, obs);
    for (k = 0; k < count; k++) {                    /* STEP 4 */
        JSValue mo = JS_GetPropertyUint32(rctx, obs, k);
        JSValue mapped = JS_GetPropertyUint32(rctx, olds, k);
        JSValue mstate = mo_state(rctx, mo), queue, rec;
        const char *mold = NULL;
        size_t mold_len = 0;

        DCHECK(!JS_IsException(mstate), "§4.3.2 collected something that is not a MutationObserver");
        if (JS_IsString(mapped)) mold = JS_ToCStringLen(rctx, &mold_len, mapped);
        /* Step 4.1: each observer gets its OWN record, and therefore its own two [SameObject] NodeLists. */
        rec = mutation_record_new(rctx, type, target,
                                  JS_IsObject(added) ? mo_clone(rctx, added) : JS_NewArray(rctx),
                                  JS_IsObject(removed) ? mo_clone(rctx, removed) : JS_NewArray(rctx),
                                  prev, next, name, ns, mold, mold_len);
        if (mold) JS_FreeCString(rctx, mold);
        queue = JS_GetPropertyUint32(rctx, mstate, MO_S_RECORDS);
        mo_push(rctx, queue, rec);                    /* step 4.2 */
        JS_FreeValue(rctx, queue);
        JS_FreeValue(rctx, mstate);
        {                                            /* step 4.3: a SET, so a repeat is not appended twice */
            uint32_t pn = mo_len(rctx, g_pending), j;
            bool have = false;
            for (j = 0; j < pn && !have; j++) {
                JSValue o = JS_GetPropertyUint32(rctx, g_pending, j);
                have = JS_VALUE_GET_PTR(o) == JS_VALUE_GET_PTR(mo);
                JS_FreeValue(rctx, o);
            }
            if (!have) mo_push(rctx, g_pending, JS_DupValue(rctx, mo));
        }
        JS_FreeValue(rctx, mapped);
        JS_FreeValue(rctx, mo);
    }
    JS_FreeValue(rctx, obs);
    JS_FreeValue(rctx, olds);
    if (count) mutation_observer_queue_microtask(rctx);              /* STEP 5 */
}

void mutation_observer_transient_for_removal(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent)
{
    lxb_dom_node_t *a;
    JSValue nwrap, nlist;

    if (!g_ready || !g_any_observer || !node || !parent) return;
    nwrap = node_wrap(ctx, node);
    if (JS_IsNull(nwrap)) { JS_FreeValue(ctx, nwrap); return; }
    nlist = JS_UNDEFINED;
    for (a = parent; a; a = a->parent) {
        JSValue wrap = node_wrap(ctx, a);
        JSValue list = JS_IsNull(wrap) ? JS_UNDEFINED : mo_ro_list(ctx, wrap, 0);
        uint32_t ln, i;

        JS_FreeValue(ctx, wrap);
        if (!JS_IsObject(list)) { JS_FreeValue(ctx, list); continue; }
        ln = mo_len(ctx, list);
        for (i = 0; i < ln; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, i);
            JSValue options = JS_GetPropertyUint32(ctx, e, RO_OPTIONS);

            if (mo_opt_true(ctx, options, "subtree")) {
                JSValue t = JS_NewArray(ctx);
                JSValue mo = JS_GetPropertyUint32(ctx, e, RO_OBSERVER);
                JSValue mstate, nodes;

                CHECK(!JS_IsException(t), "a transient registered observer could not be allocated");
                JS_SetPropertyUint32(ctx, t, RO_OBSERVER, JS_DupValue(ctx, mo));
                JS_SetPropertyUint32(ctx, t, RO_OPTIONS, JS_DupValue(ctx, options));
                JS_SetPropertyUint32(ctx, t, RO_SOURCE, JS_DupValue(ctx, e));   /* what makes it transient */
                if (!JS_IsObject(nlist)) nlist = mo_ro_list(ctx, nwrap, 1);
                mo_push(ctx, nlist, t);
                /* AND THE NODE JOINS THE OBSERVER'S NODE LIST. §4.3's notify step 6.3 and `disconnect()` both
                   reach a transient registration by walking "each node of mo's node list" — a registration on
                   a node that list does not name is one nothing can ever remove, so it would outlive both the
                   notification that is supposed to drop it and the disconnect that is supposed to end it. */
                mstate = mo_state(ctx, mo);
                DCHECK(!JS_IsException(mstate), "a registered observer names something that is not a "
                                                "MutationObserver");
                nodes = JS_GetPropertyUint32(ctx, mstate, MO_S_NODES);
                {
                    uint32_t nn = mo_len(ctx, nodes), j;
                    bool have = false;
                    for (j = 0; j < nn && !have; j++) {
                        JSValue w = JS_GetPropertyUint32(ctx, nodes, j);
                        have = JS_VALUE_GET_PTR(w) == JS_VALUE_GET_PTR(nwrap);
                        JS_FreeValue(ctx, w);
                    }
                    if (!have) mo_push(ctx, nodes, JS_DupValue(ctx, nwrap));
                }
                JS_FreeValue(ctx, nodes);
                JS_FreeValue(ctx, mstate);
                JS_FreeValue(ctx, mo);
            }
            JS_FreeValue(ctx, options);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, list);
    }
    JS_FreeValue(ctx, nlist);
    JS_FreeValue(ctx, nwrap);
}

/* §4.2.3's `suppressObservers`, as the SCOPE the algorithm actually has rather than a parameter this engine
 * cannot thread. Inserting a DocumentFragment is ONE operation in the standard and N tree writes here: step 1
 * makes `nodes` the fragment's children, step 4 removes them from the fragment WITH suppressObservers SET TO
 * TRUE, and the last step queues ONE record for the parent with the whole list. The per-node chokepoint hook
 * cannot see that shape — it fires once per child — so it queued N records where a browser queues one, which
 * is exactly what the eight remaining MutationObserver-childList failures say ("expected 1 but got 2").
 *
 * Accumulating in the OBSERVER rather than passing a flag down through the tree chokepoint is what keeps
 * `node_insert_at` free of a JSContext it does not have: the hook already receives one per call, and the two
 * aggregate records are emitted at the end from the context that was actually running.
 *
 * THE OPERATION'S PARENT IS GIVEN, NOT DISCOVERED, AND SO IS THE ONE REMOVAL IT SUPPRESSES. This scope used to
 * infer both — the parent from whichever insertion came first, the "removals belong to the fragment" from the
 * fact that insert step 4 is the only place it was opened — and that inference is wrong the moment §4.2.3's
 * OTHER algorithm uses it. `replace` removes `child` FROM THE OPERATION'S OWN PARENT with suppressObservers
 * set, and queues ONE record naming BOTH lists; `insert` removes a fragment's children from the FRAGMENT and
 * queues a second record for it. Told the parent, this scope tells the three removals apart by where they come
 * from:
 *   - from the operation's parent AND the node the operation suppresses  → the operation's own removedNodes;
 *   - from anywhere else inside the scope                                → step 4's record on that fragment;
 *   - from the operation's parent but some OTHER node                    → NOT this operation. That is adopt's
 *     own step 2 removing the inserted node from a parent it already had, which the standard queues as its own
 *     record, in order, before this one — so it is handed back to the per-node path rather than absorbed.
 * Inferring it instead is what made `p.replaceChild(x, c)` queue two records where a browser queues one, and
 * would have made `p.replaceChild(x, c)` for an `x` already in `p` merge two operations into one.
 *
 * A run with no observer registered never reaches here at all, so the scope is inert until someone is watching. */
static int      g_batch;
static JSContext *g_batch_ctx;
static JSValue  g_batch_add = JS_UNDEFINED, g_batch_rem = JS_UNDEFINED, g_batch_frem = JS_UNDEFINED;
static uint32_t g_batch_nadd, g_batch_nrem, g_batch_nfrem;
static lxb_dom_node_t *g_batch_parent, *g_batch_suppressed, *g_batch_from, *g_batch_prev, *g_batch_next;

void mutation_observer_batch_begin(lxb_dom_node_t *parent, lxb_dom_node_t *suppressed, lxb_dom_node_t *from)
{
    DCHECK(parent != NULL, "§4.2.3's insert and replace each have exactly one parent, and this scope was opened "
                           "without it — the target of the record it will queue is not a thing to discover");
    if (g_batch++ == 0) {
        g_batch_nadd = g_batch_nrem = g_batch_nfrem = 0;
        g_batch_parent = parent;
        g_batch_suppressed = suppressed;
        g_batch_from = from;
        g_batch_prev = g_batch_next = NULL;
        g_batch_ctx = NULL;
        return;
    }
    /* NESTED: `parent.replaceChild(fragment, child)` opens replace's scope and then insert's inside it. It is
       ONE operation with one record, so the inner scope must not start a second — but insert's scope is where
       the FRAGMENT is known, so that half is adopted rather than discarded. */
    DCHECK(parent == g_batch_parent,
           "a mutation-record scope was nested inside one naming a different parent — §4.2.3's replace opens "
           "insert's scope for the same parent, and two parents here means two operations sharing one record");
    DCHECK(suppressed == NULL || suppressed == g_batch_suppressed,
           "a nested mutation-record scope named a different suppressed child — only replace suppresses one, "
           "and it is the outermost of the two scopes");
    if (from && !g_batch_from) g_batch_from = from;
}

void mutation_observer_batch_end(void)
{
    JSContext *ctx = g_batch_ctx;

    DCHECK(g_batch > 0, "a mutation-record batch ended that never began");
    if (--g_batch > 0) return;
    if (!ctx) return;                       /* nothing accumulated: no observer, or an empty fragment */
    /* Insert step 4.2's record on the FRAGMENT, then the operation's own record on the PARENT — in that order,
       because the standard queues them in that order and §4.3's queue is observably ordered. */
    if (g_batch_nfrem)
        mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, g_batch_from, NULL, NULL, NULL, 0,
                                       JS_UNDEFINED, g_batch_frem, NULL, NULL);
    /* ONE RECORD, BOTH LISTS. Insert's is « » removed and replace's is « child »; the standard queues the same
       record from both algorithms and the only difference is what is in the two lists. */
    if (g_batch_nadd || g_batch_nrem)
        mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, g_batch_parent, NULL, NULL, NULL, 0,
                                       g_batch_add, g_batch_rem, g_batch_prev, g_batch_next);
    JS_FreeValue(ctx, g_batch_add); g_batch_add = JS_UNDEFINED;
    JS_FreeValue(ctx, g_batch_rem); g_batch_rem = JS_UNDEFINED;
    JS_FreeValue(ctx, g_batch_frem); g_batch_frem = JS_UNDEFINED;
    g_batch_ctx = NULL;
}

/* One node joins the scope's list. Returns true when it was taken, which is what tells the caller not to queue
   the per-node record the standard does not have. */
static bool batch_take(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int inserted, JSValue wrap)
{
    JSValue *list;
    uint32_t *count;

    if (!g_batch) return false;
    if (inserted) {
        DCHECK(g_batch_parent == parent,
               "one mutation-record batch spanned two insertion parents — §4.2.3's insert has exactly one, so "
               "this scope is wrapped around something that is not one insert");
        list = &g_batch_add; count = &g_batch_nadd;
    } else if (n == g_batch_suppressed && parent == g_batch_parent) {
        list = &g_batch_rem; count = &g_batch_nrem;          /* replace step 7's suppressed removal */
    } else if (parent == g_batch_from) {
        list = &g_batch_frem; count = &g_batch_nfrem;        /* insert step 4's, from the fragment */
    } else {
        /* NOT THIS OPERATION. Adopt's own step 2 removing the inserted node from a parent it already had —
           the standard queues that as its own record, in order, with the old siblings this scope does not
           carry, so it goes back to the per-node path rather than being folded in here. */
        return false;
    }
    if (!g_batch_ctx) g_batch_ctx = ctx;
    if (JS_IsUndefined(*list)) {
        *list = JS_NewArray(ctx);
        CHECK(!JS_IsException(*list), "a batched tree mutation record's node list could not be allocated");
    }
    if (inserted) {
        /* The operation's position is the FIRST insertion's previous sibling and the LAST one's next — which,
           because replace's suppressed removal has already happened, is child's previous sibling and the
           referenceChild the standard binds at its steps 2 and 4. */
        if (!g_batch_nadd) g_batch_prev = n->prev;
        g_batch_next = n->next;
    }
    JS_SetPropertyUint32(ctx, *list, (*count)++, wrap);
    return true;
}

void mutation_observer_tree_steps(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    JSValue list, wrap;
    int inserted = phase == NODE_TREE_INSERTED;

    if (!g_ready || !g_any_observer || !n) return;
    /* NODE_TREE_REMOVED is the phase AFTER the detach, which exists for the slot steps: §4.2.3's removal record
       carries oldPreviousSibling and oldNextSibling, bound at steps 5-6 before the node leaves, so by then
       there is nothing left to read and nothing for this to say. */
    if (phase != NODE_TREE_INSERTED && phase != NODE_TREE_REMOVING) return;
    /* THE TARGET IS THE PARENT, which is what makes a node with none not a childList mutation of anything —
       the chokepoint is also how a flow-private tree is built, and a node being attached to nothing has no
       registered observer list above it to reach. */
    if (!parent) return;
    if (!inserted)                                  /* remove STEP 15, before step 16's record */
        mutation_observer_transient_for_removal(ctx, n, parent);
    wrap = node_wrap(ctx, n);
    if (JS_IsNull(wrap)) { JS_FreeValue(ctx, wrap); return; }
    /* Inside §4.2.3's scope this node is one entry of ONE record, not a record of its own. */
    if (batch_take(ctx, n, parent, inserted, wrap)) return;
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a tree mutation record's node list could not be allocated");
    JS_SetPropertyUint32(ctx, list, 0, wrap);
    /* "QUEUE A TREE MUTATION RECORD for target with addedNodes, removedNodes, previousSibling and
       nextSibling" — §4.3.2's own wrapper, which is a `childList` record with null name, namespace and old
       value. Its assert ("either addedNodes or removedNodes is not empty") holds by construction here: one of
       the two IS this node. */
    mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, parent, NULL, NULL, NULL, 0,
                                   inserted ? list : JS_UNDEFINED,
                                   inserted ? JS_UNDEFINED : list,
                                   n->prev, n->next);
    JS_FreeValue(ctx, list);
}

void mutation_observer_move_steps(JSContext *ctx, lxb_dom_node_t *node,
                                  lxb_dom_node_t *old_parent, lxb_dom_node_t *old_prev, lxb_dom_node_t *old_next,
                                  lxb_dom_node_t *new_parent, lxb_dom_node_t *new_prev, lxb_dom_node_t *child)
{
    JSValue one, wrap;

    DCHECK(node && old_parent && new_parent,
           "§4.2.3's move queued its records without one of the three nodes they are about — steps 25 and 26 "
           "name oldParent, newParent and node, and step 8 asserts oldParent is non-null");
    if (!g_ready || !g_any_observer) return;
    wrap = node_wrap(ctx, node);
    if (JS_IsNull(wrap)) { JS_FreeValue(ctx, wrap); return; }
    one = JS_NewArray(ctx);
    CHECK(!JS_IsException(one), "a move's tree mutation record node list could not be allocated");
    JS_SetPropertyUint32(ctx, one, 0, wrap);
    /* STEP 25 — "queue a tree mutation record for oldParent with « », « node », oldPreviousSibling, and
       oldNextSibling". The siblings are the ones step 11-12 bound BEFORE the detach, so they arrive as
       arguments rather than being read back off a node that has since been re-parented. */
    mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, old_parent, NULL, NULL, NULL, 0,
                                   JS_UNDEFINED, one, old_prev, old_next);
    /* STEP 26 — "queue a tree mutation record for newParent with « node », « », newPreviousSibling, and
       child". `child` is the member's own reference child and not `node`'s current next sibling: they are the
       same node here, but the standard names the argument, and a re-derivation would answer differently the
       moment `child` is null (an append, whose nextSibling is null either way) or the tree moves under it. */
    mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, new_parent, NULL, NULL, NULL, 0,
                                   one, JS_UNDEFINED, new_prev, child);
    JS_FreeValue(ctx, one);
}

void mutation_observer_character_data(JSContext *ctx, lxb_dom_node_t *node, const char *old, size_t old_len)
{
    if (!g_ready || !g_any_observer || !node) return;
    mutation_observer_queue_record(ctx, MR_TYPE_CHARACTER_DATA, node, NULL, NULL, old, old_len,
                                   JS_UNDEFINED, JS_UNDEFINED, NULL, NULL);
}

/* ---- §4.3.1's THREE METHODS ------------------------------------------------------------------------------ */

/* `observe(target, options)`. Steps 1-6 are the validation — six branches, four of which are TypeErrors, and
   they are the whole reason MutationObserverInit's booleans must know absence from false. No page code runs
   in any of them: the declaration converted the dictionary before step 1, which is where a getter on the
   page's options object ran. */
static JSValue js_mo_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = mo_state(ctx, this_val), options, list, nodes;
    lxb_dom_node_t *target;
    uint32_t ln, i;
    bool found = false;

    (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(argc >= 1, "observe reached its body without the argument its declaration requires");
    target = node_of(argv[0]);
    DCHECK(target != NULL, "observe's `Node target` reached the body as something that is not a node — the "
                           "declaration's interface type is what makes a non-Node a TypeError before step 1");
    /* `optional MutationObserverInit options = {}`. A POSITION THE CALL DID NOT PASS IS NOT CONVERTED — the
       args machine converts exactly `min(argc, nargs)` — so the IDL's `= {}` default is built here, as the
       empty dictionary it is. Every member is then ABSENT, which is precisely what makes `observe(target)`
       fall to step 3's TypeError rather than registering an observer that reports nothing. */
    options = argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_NewObject(ctx);
    DCHECK(JS_IsObject(options), "observe's options reached the body as something that is not the object the "
                                 "dictionary conversion builds");

    if (mo_opt_exists(ctx, options, "attributeOldValue") ||                       /* step 1 */
        mo_opt_exists(ctx, options, "attributeFilter")) {
        if (!mo_opt_exists(ctx, options, "attributes"))
            JS_SetPropertyStr(ctx, options, "attributes", JS_TRUE);
    }
    if (mo_opt_exists(ctx, options, "characterDataOldValue") &&                   /* step 2 */
        !mo_opt_exists(ctx, options, "characterData"))
        JS_SetPropertyStr(ctx, options, "characterData", JS_TRUE);

    if (!mo_opt_true(ctx, options, "childList") && !mo_opt_true(ctx, options, "attributes") &&
        !mo_opt_true(ctx, options, "characterData")) {                            /* step 3 */
        JS_FreeValue(ctx, options);
        JS_FreeValue(ctx, state);
        return JS_ThrowTypeError(ctx, "one of childList, attributes or characterData must be true");
    }
    if (mo_opt_true(ctx, options, "attributeOldValue") && !mo_opt_true(ctx, options, "attributes")) {
        JS_FreeValue(ctx, options);                                               /* step 4 */
        JS_FreeValue(ctx, state);
        return JS_ThrowTypeError(ctx, "attributeOldValue requires attributes");
    }
    if (mo_opt_exists(ctx, options, "attributeFilter") && !mo_opt_true(ctx, options, "attributes")) {
        JS_FreeValue(ctx, options);                                               /* step 5 */
        JS_FreeValue(ctx, state);
        return JS_ThrowTypeError(ctx, "attributeFilter requires attributes");
    }
    if (mo_opt_true(ctx, options, "characterDataOldValue") &&
        !mo_opt_true(ctx, options, "characterData")) {                            /* step 6 */
        JS_FreeValue(ctx, options);
        JS_FreeValue(ctx, state);
        return JS_ThrowTypeError(ctx, "characterDataOldValue requires characterData");
    }

    g_any_observer = true;
    list = mo_ro_list(ctx, argv[0], 1);
    nodes = JS_GetPropertyUint32(ctx, state, MO_S_NODES);
    ln = mo_len(ctx, list);
    for (i = 0; i < ln; i++) {                                                    /* step 7 */
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        JSValue mo = JS_GetPropertyUint32(ctx, e, RO_OBSERVER);

        if (JS_VALUE_GET_PTR(mo) == JS_VALUE_GET_PTR(this_val)) {
            uint32_t nn = mo_len(ctx, nodes), k;

            found = true;
            /* Step 7.1.1: every transient registration SOURCED at this one goes, across every node this
               observer watches — re-observing a target is what ends the transient window early. */
            for (k = 0; k < nn; k++) {
                JSValue w = JS_GetPropertyUint32(ctx, nodes, k);
                JSValue wl = mo_ro_list(ctx, w, 0);
                uint32_t wn, j, out = 0;

                if (JS_IsObject(wl)) {
                    wn = mo_len(ctx, wl);
                    for (j = 0; j < wn; j++) {
                        JSValue te = JS_GetPropertyUint32(ctx, wl, j);
                        JSValue src = JS_GetPropertyUint32(ctx, te, RO_SOURCE);
                        bool drop = JS_VALUE_GET_PTR(src) == JS_VALUE_GET_PTR(e);

                        JS_FreeValue(ctx, src);
                        if (drop) { JS_FreeValue(ctx, te); continue; }
                        JS_SetPropertyUint32(ctx, wl, out++, te);
                    }
                    if (out != wn) mo_set_len(ctx, wl, out);
                }
                JS_FreeValue(ctx, wl);
                JS_FreeValue(ctx, w);
            }
            JS_SetPropertyUint32(ctx, e, RO_OPTIONS, JS_DupValue(ctx, options));   /* step 7.1.2 */
        }
        JS_FreeValue(ctx, mo);
        JS_FreeValue(ctx, e);
    }
    if (!found) {                                                                  /* step 7.2 */
        JSValue e = JS_NewArray(ctx);

        CHECK(!JS_IsException(e), "a registered observer could not be allocated");
        JS_SetPropertyUint32(ctx, e, RO_OBSERVER, JS_DupValue(ctx, this_val));
        JS_SetPropertyUint32(ctx, e, RO_OPTIONS, JS_DupValue(ctx, options));
        JS_SetPropertyUint32(ctx, e, RO_SOURCE, JS_UNDEFINED);
        mo_push(ctx, list, e);
        mo_push(ctx, nodes, JS_DupValue(ctx, argv[0]));
    }
    JS_FreeValue(ctx, nodes);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, options);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* `disconnect()`: drop every registration this observer holds, transient ones included, and empty its queue. */
static JSValue js_mo_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = mo_state(ctx, this_val), nodes, queue;
    uint32_t nn, k;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    nodes = JS_GetPropertyUint32(ctx, state, MO_S_NODES);
    nn = mo_len(ctx, nodes);
    for (k = 0; k < nn; k++) {                                                     /* step 1 */
        JSValue w = JS_GetPropertyUint32(ctx, nodes, k);
        JSValue l = mo_ro_list(ctx, w, 0);
        uint32_t ln, i, out = 0;

        if (JS_IsObject(l)) {
            ln = mo_len(ctx, l);
            for (i = 0; i < ln; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, l, i);
                JSValue mo = JS_GetPropertyUint32(ctx, e, RO_OBSERVER);
                bool drop = JS_VALUE_GET_PTR(mo) == JS_VALUE_GET_PTR(this_val);

                JS_FreeValue(ctx, mo);
                if (drop) { JS_FreeValue(ctx, e); continue; }
                JS_SetPropertyUint32(ctx, l, out++, e);
            }
            if (out != ln) mo_set_len(ctx, l, out);
        }
        JS_FreeValue(ctx, l);
        JS_FreeValue(ctx, w);
    }
    mo_set_len(ctx, nodes, 0);
    JS_FreeValue(ctx, nodes);
    queue = JS_NewArray(ctx);                                                      /* step 2 */
    CHECK(!JS_IsException(queue), "a MutationObserver's record queue could not be re-allocated");
    JS_SetPropertyUint32(ctx, state, MO_S_RECORDS, queue);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

static JSValue js_mo_take_records(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = mo_state(ctx, this_val), queue, out;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    queue = JS_GetPropertyUint32(ctx, state, MO_S_RECORDS);
    out = mo_clone(ctx, queue);                                                    /* step 1 */
    JS_FreeValue(ctx, queue);
    JS_SetPropertyUint32(ctx, state, MO_S_RECORDS, JS_NewArray(ctx));              /* step 2 */
    JS_FreeValue(ctx, state);
    return out;                                                                    /* step 3 */
}

/* ---- §4.3.1's CONSTRUCTOR --------------------------------------------------------------------------------- */

typedef struct { uint8_t unused; } JSMoCtorState;
static void js_mo_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define MO_CTOR_STAGES(X) \
    X(MO_CTOR_BUILD = IDL_STEP_FIRST, \
      "DOM §4.3.1 new MutationObserver(callback) (\"set this's callback to callback\"), with its node list and " \
      "record queue empty")
enum { MO_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const MO_CTOR_STEPS[] = { MO_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_mo_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, state, proto;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == MO_CTOR_BUILD, "the MutationObserver constructor resumed at a stage §4.3.1 does not "
                                        "have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor MutationObserver requires 'new'"), -1;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "MutationObserver requires a callback"), -1;
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "MutationObserver's callback reached the body unconverted — §4.3.1 declares it a MutationCallback, "
           "and Web IDL §3.2.19's brand test is what makes a non-callable a TypeError before step 1");
    proto = JS_GetClassProto(ctx, g_mo_class);
    DCHECK(!JS_IsNull(proto), "a MutationObserver was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_mo_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return -1;
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "a MutationObserver's state could not be allocated");
    JS_SetPropertyUint32(ctx, state, MO_S_CALLBACK, JS_DupValue(ctx, argv[0]));
    JS_SetPropertyUint32(ctx, state, MO_S_NODES, JS_NewArray(ctx));
    JS_SetPropertyUint32(ctx, state, MO_S_RECORDS, JS_NewArray(ctx));
    JS_DefinePropertyValue(ctx, obj, g_atom_mo, state, 0);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_mo_ctor_decl = {
    js_mo_ctor_step, sizeof(JSMoCtorState), js_mo_ctor_visit, NULL,
    "DOM §4.3.1 new MutationObserver(callback)", MO_CTOR_STEPS
};

/* ---- declaration and installation ------------------------------------------------------------------------ */

void mutation_observer_init(JSContext *ctx)
{
    /* §4.3.1's MutationObserverInit, MEMBER FOR MEMBER AND DEFAULT FOR DEFAULT. Four of the seven declare NO
       default, and that is the type stating something observe() reads: `attributes` absent is not `attributes`
       false, and IDL_BOOLEAN_NO_DEFAULT is what keeps the two apart all the way to step 5's TypeError.
       Web IDL reads a dictionary's members in LEXICOGRAPHIC order, which is the order written here. */
    static const IdlDictMember INIT_MEMBERS[] = {
        { "attributeFilter",        IDL_SEQUENCE_DOMSTRING },
        { "attributeOldValue",      IDL_BOOLEAN_NO_DEFAULT },
        { "attributes",             IDL_BOOLEAN_NO_DEFAULT },
        { "characterData",          IDL_BOOLEAN_NO_DEFAULT },
        { "characterDataOldValue",  IDL_BOOLEAN_NO_DEFAULT },
        { "childList",              IDL_BOOLEAN },   /* `= false` */
        { "subtree",                IDL_BOOLEAN },   /* `= false` */
    };
    static const IdlArgType OBSERVE_ARGS[2] = { IDL_INTERFACE, IDL_DICT };
    static const IdlArgType CTOR_ARGS[1] = { IDL_CALLBACK };
    JSClassDef d = { "MutationObserver" };

    if (g_ready) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_mo_class);
    JS_NewClass(JS_GetRuntime(ctx), g_mo_class, &d);
    mutation_record_init(ctx);

    g_mo_key = JS_NewSymbol(ctx, "mutationObserverState", false);
    CHECK(!JS_IsException(g_mo_key), "the MutationObserver state slot key allocation failed");
    g_atom_mo = JS_ValueToAtom(ctx, g_mo_key);
    g_ro_key = JS_NewSymbol(ctx, "registeredObserverList", false);
    CHECK(!JS_IsException(g_ro_key), "the registered observer list slot key allocation failed");
    g_atom_ro = JS_ValueToAtom(ctx, g_ro_key);
    g_atom_queued = JS_NewAtom(ctx, "queued");
    CHECK(g_atom_mo != JS_ATOM_NULL && g_atom_ro != JS_ATOM_NULL && g_atom_queued != JS_ATOM_NULL,
          "a §4.3 slot key could not be interned");

    g_pending = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_pending), "the agent's pending mutation observers could not be allocated");
    JS_SetProperty(ctx, g_pending, g_atom_queued, JS_FALSE);
    g_notify_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_mo_notify_def);
    CHECK(g_notify_stepid >= 0, "no step id for §4.3's notification driver");
    g_notify_slot = realm_value_declare(ctx, "§4.3 notifyMutationObservers");

    g_id_ctor = idl_method_id_step(ctx, CTOR_ARGS, 1, NULL, 0, &js_mo_ctor_decl, 0);
    g_id_observe = idl_method_id_dict(ctx, OBSERVE_ARGS, 2, INIT_MEMBERS,
                                      (int)(sizeof(INIT_MEMBERS) / sizeof(INIT_MEMBERS[0])),
                                      js_mo_observe, 0);
    idl_iface_brand(node_class_id());
    idl_optional_from(1);   /* §4.3.1: `observe(Node target, optional MutationObserverInit options = {})` */
    g_id_disconnect = idl_method_id(ctx, NULL, 0, js_mo_disconnect, 0);
    g_id_take = idl_method_id(ctx, NULL, 0, js_mo_take_records, 0);

    realm_declare_intrinsic(mutation_observer_install_proto);
    g_ready = 1;
}

void mutation_observer_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_mo_class != 0, "a realm asked for MutationObserver.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_mo_class);
    DCHECK(JS_IsNull(prev), "mutation_observer_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    {
        /* THE NOTIFICATION DRIVER IS THIS REALM'S. A function object carries the realm it was minted in, and
           this one fires the page's `slotchange` listeners and invokes its observer callbacks — a driver held
           in one static would run every document's notification out of whichever realm built it first. It is a
           step function object nobody installs, so a page can neither see it nor replace it. */
        JSValue fn = JS_NewCFunction2(ctx, NULL, "notifyMutationObservers", 0, JS_CFUNC_step, g_notify_stepid);
        CHECK(!JS_IsException(fn), "§4.3's notification driver could not be allocated");
        realm_value_set(ctx, g_notify_slot, fn);
    }
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "MutationObserver.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MutationObserver");
    idl_install_method(ctx, proto, "observe", g_id_observe);
    idl_install_method(ctx, proto, "disconnect", g_id_disconnect);
    idl_install_method(ctx, proto, "takeRecords", g_id_take);
    JS_SetClassProto(ctx, g_mo_class, proto);
}

void mutation_observer_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    mutation_record_install(ctx, global);
    DCHECK(g_id_ctor >= 0, "MutationObserver was installed before mutation_observer_init declared it");
    ctor = idl_step_constructor(ctx, "MutationObserver", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the MutationObserver interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_mo_class);
    DCHECK(!JS_IsNull(proto), "MutationObserver was installed in a realm that never ran its prototype install");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MutationObserver", ctor);
}

void mutation_observer_free(JSRuntime *rt)
{
    if (!g_ready) return;
    mutation_record_free(rt);
    JS_FreeValueRT(rt, g_pending);
    JS_FreeValueRT(rt, g_mo_key);
    JS_FreeValueRT(rt, g_ro_key);
    g_pending = g_mo_key = g_ro_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_mo);
    JS_FreeAtomRT(rt, g_atom_ro);
    JS_FreeAtomRT(rt, g_atom_queued);
    g_atom_mo = g_atom_ro = g_atom_queued = JS_ATOM_NULL;
    g_notify_stepid = g_notify_slot = -1;
    g_id_observe = g_id_disconnect = g_id_take = g_id_ctor = -1;
    g_any_observer = false;
    g_ready = 0;
}
