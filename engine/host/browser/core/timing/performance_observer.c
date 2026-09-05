/* PERFORMANCE TIMELINE §4 The PerformanceObserver interface, §5.1 Queue a PerformanceEntry and §5.3 Queue the
 * PerformanceObserver task. See performance_observer.h for what is built, what is not, and why §4.5's list is
 * derived from the components that mint entries rather than typed here. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/events/report_exception.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/timing/performance_entry.h"
#include "core/timing/performance_observer.h"
#include "core/timing/performance_observer_entry_list.h"

/* §4's FOUR ASSOCIATED CONCEPTS, as one Array in an own slot on the observer. It is a JS Array and not a C
   struct for the reason core/events/message_port.c gives about a port's queue: an Array's mutations are
   property writes the per-flow COW delta already captures, so one forked arm's `observe` is invisible to its
   sibling and the whole record parks and resumes with the flow. */
enum { PO_S_CALLBACK = 0, PO_S_BUFFER, PO_S_TYPE, PO_S_DROPPED, PO_S_COUNT };

/* §4's OBSERVER TYPE. The standard calls it "a DOMString observer type which is initially "undefined"" and
   gives it exactly three values; nothing exposes it to a page, so it is held as the index of one of those
   three rather than as a string it would have to be compared back out of. */
typedef enum { PO_TYPE_UNDEFINED = 0, PO_TYPE_SINGLE, PO_TYPE_MULTIPLE } PoObserverType;

/* §4's "registered performance observer ... a struct consisting of an observer member ... and an options list
   member (a list of PerformanceObserverInit dictionaries)", as a two-slot Array. The LIST those records go in
   is §2 "Performance Timeline"'s, which is where that section gives every global object "a list of registered
   performance observer objects that is initially empty". */
enum { PO_R_OBSERVER = 0, PO_R_OPTIONS, PO_R_COUNT };

static JSClassID g_class;
static JSValue   g_state_key = JS_UNDEFINED;
static JSAtom    g_atom_state = JS_ATOM_NULL;
/* §5.3's "performance observer task queued flag", carried as a named property ON the realm's registered-observer
   list — the same place core/intersection_observer/intersection_observer.c keeps §3.1.1's, and for the same
   reason: it is per-global state a flow writes, so it must ride a value the COW delta already captures. */
static JSAtom    g_atom_queued = JS_ATOM_NULL;
static int       g_reg_slot = -1;      /* this realm's §2 list of registered performance observer objects */
static int       g_notify_slot = -1;   /* this realm's §5.3 task callee */
static int       g_types_slot = -1;    /* this realm's §4.5 frozen array of supported entry types */
static int       g_notify_stepid = -1;
static int       g_id_ctor = -1, g_id_observe = -1, g_id_take = -1, g_id_disconnect = -1;
static int       g_ready;

/* §4.5's SET, DECLARED BY THE PRODUCERS. See the header for why it is not a list in this file. The cap is a
   CHECK rather than a growable buffer because the whole population is the entry types this build can mint,
   which is bounded by the number of timing standards the engine implements and is nowhere near this. */
#define PO_MAX_ENTRY_TYPES 32
static const char *g_entry_types[PO_MAX_ENTRY_TYPES];
static int         g_n_entry_types;

void performance_observer_declare_entry_type(const char *name)
{
    int i;

    DCHECK(name != NULL && *name != '\0',
           "an entry type was declared with no name — §4.5's array holds the registry's own key for a type, "
           "and a producer that cannot name its type has nothing to declare");
    for (i = 0; i < g_n_entry_types; i++)
        DCHECK(strcmp(g_entry_types[i], name) != 0,
               "one entry type was declared by two producers — §4.5's array answers WHICH TYPES THIS BUILD CAN "
               "MINT, so two declarations are two answers to one question and §5.1 step 7.1 would then have two "
               "producers it cannot tell apart");
    CHECK(g_n_entry_types < PO_MAX_ENTRY_TYPES,
          "more entry types were declared than §4.5's array can hold");
    g_entry_types[g_n_entry_types++] = name;
}

/* ---- small list helpers ----------------------------------------------------------------------------------- */

static uint32_t po_len(JSContext *ctx, JSValueConst arr)
{
    JSValue v = JS_GetPropertyStr(ctx, (JSValue)arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void po_push(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, po_len(ctx, arr), v);
}

/* A fresh Array holding the same members, in the same order — §5.3 step 3.2's "a copy of" and step 3.3.2's. */
static JSValue po_clone(JSContext *ctx, JSValueConst arr)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = po_len(ctx, arr), i;

    CHECK(!JS_IsException(out), "a Performance Timeline list copy could not be allocated");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, (JSValue)arr, i));
    return out;
}

/* THIS REALM'S §2 "Performance Timeline" LIST OF REGISTERED PERFORMANCE OBSERVER OBJECTS. OWNED. */
static JSValue po_reg_list(JSContext *ctx)
{
    JSValue list = realm_value_get(ctx, g_reg_slot);

    DCHECK(JS_IsArray(list),
           "a realm was asked for §2's list of registered performance observer objects before "
           "performance_observer_install_protos built one — it is built EAGERLY with the realm precisely so "
           "that no flow's own `observe` is what creates it");
    return list;
}

/* THE OBSERVER'S OWN STATE. OWNED, or an exception. */
static JSValue po_state(JSContext *ctx, JSValueConst v)
{
    JSValue s;

    /* WEB IDL §3.7.6/§3.7.9's BRAND TEST, AS A REAL TypeError. The receiver is PAGE-SUPPLIED INPUT —
       `PerformanceObserver.prototype.disconnect.call(null)` is one line, and a forcing solver writes unusual
       receivers constantly — so a DCHECK here would hand any page an abort switch for the engine. */
    if (JS_GetClassID(v) != g_class)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (JS_GetOwnSlot(ctx, &s, v, g_atom_state) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsArray(s),
           "a PerformanceObserver carries no state — §4's constructor writes every one of its four concepts "
           "before the object exists, so one without them was built somewhere that is not this file");
    return s;
}

/* The record in this realm's list whose observer is `obs`, or -1 — §4.2 steps 6.4 and 7.3's "contains a
   registered performance observer whose observer is this". Identity, because §4's struct names the OBJECT. */
static int po_registration_of(JSContext *ctx, JSValueConst list, JSValueConst obs)
{
    uint32_t n = po_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, (JSValue)list, i);
        JSValue o = JS_GetPropertyUint32(ctx, rec, PO_R_OBSERVER);
        bool same = JS_VALUE_GET_PTR(o) == JS_VALUE_GET_PTR(obs);

        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, rec);
        if (same) return (int)i;
    }
    return -1;
}

/* §4.5's array, for the membership test §4.2 steps 6.2 and 7.2 make of it — §4.2's own words are "contained
   in relevantGlobal's frozen array of supported entry types".
   `name` is a JS string the declaration produced (a DOMString member), never a C string, so the comparison is
   the interpreter's rather than a strcmp over two copies. */
static bool po_type_supported(JSContext *ctx, JSValueConst name)
{
    JSValue types = realm_value_get(ctx, g_types_slot);
    uint32_t n, i;
    bool found = false;

    DCHECK(JS_IsArray(types), "a realm was asked for §4.5's frozen array before its install built one");
    n = po_len(ctx, types);
    for (i = 0; i < n && !found; i++) {
        JSValue t = JS_GetPropertyUint32(ctx, types, i);

        found = JS_IsStrictEqual(ctx, t, name);
        JS_FreeValue(ctx, t);
    }
    JS_FreeValue(ctx, types);
    return found;
}

/* THE TIMING ENTRY TYPES REGISTRY'S "should add entry" FOR THE TYPES THIS BUILD MINTS.
 *
 * The registry makes the algorithm a per-entry-type row: "Each entry type must specify an algorithm should add
 * entry which, given a PerformanceEntry entry of the entry type and optionally a PerformanceObserverInit
 * options of an observer observing that entry type, determines the following: If options was not provided,
 * whether entry is eligible to be added to the performance timeline, regardless of limitations in the buffer
 * size." The registry's row for "mark" — the ONE type this build declares a producer for — reads `Return true`,
 * so this is that row and not a placeholder for one. The assert is what stops it silently becoming a
 * placeholder: a type declared by some later producer reaches here with a row this function has never read. */
static bool po_should_add_entry(JSContext *ctx, JSValueConst entry_type)
{
    DCHECK(po_type_supported(ctx, entry_type),
           "§5.1 step 7.1.1 asked `should add entry` of an entry type no producer in this build declared — the "
           "algorithm is a ROW OF THE TIMING ENTRY TYPES REGISTRY per type, and this function answers the one "
           "row the declared types have (`mark`: \"Return true\"). A new producer states its own row here");
    return true;
}

/* ---- §5.3 QUEUE THE PERFORMANCEOBSERVER TASK -------------------------------------------------------------- */

#define PO_NOTIFY_STAGES(X)                                                                                   \
    X(PO_NOTIFY_CALLBACK, "PERFORMANCE TIMELINE §5.3 step 3.3.9 (invoking po's observer callback with "        \
                          "« observerEntryList, po, callbackOptions » and po as the callback this value), "    \
                          "one observer per rest")                                                             \
    X(PO_NOTIFY_REPORT,   "PERFORMANCE TIMELINE §5.3 step 3.3.9's \"report\" callback-this-value behaviour — " \
                          "Web IDL §3.12's report-an-exception, which is HTML §8.1.4.6")
enum { PO_NOTIFY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PO_NOTIFY_STEPS[] = { PO_NOTIFY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;        /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   notify;     /* step 3.2's copy of the registered-observer list */
    uint32_t  i;
    JSValue   cur;        /* the observer whose callback is in flight (owned) */
    JSValue   entry_list; /* step 3.3.5's PerformanceObserverEntryList, held across the park */
    JSValue   cbopts;     /* step 3.3.8's PerformanceObserverCallbackOptions */
    uint8_t   phase;      /* step_call_run's own */
    /* HAVE THE OWNED FIELDS BEEN PLACED YET — its own byte and not a JS_IsUndefined test, because a step state
       arrives zeroed and a ZEROED JSValue is the INTEGER 0, so every value on a fresh state reads as set. */
    uint8_t   started;
    uint8_t   reporting;
    JSValue   exc;
    ReportExceptionWork rep;
    JSValue   cb[5];      /* [this, callback, entryList, observer, callbackOptions] */
} PoNotifyTask;

static void js_po_notify_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PoNotifyTask *s = st;
    int k;

    v->val(ctx, &s->notify);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->entry_list);
    v->val(ctx, &s->cbopts);
    v->val(ctx, &s->exc);
    report_exception_work_visit(ctx, &s->rep, v);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static JSValue js_po_notify_fini(JSContext *ctx, void *st, bool take_result)
{
    PoNotifyTask *s = st;

    (void)take_result;
    /* HTML §8.1.4.6 step 5's FLAG, if a report was abandoned holding it. Not a reference, so no declaration
       names it and the visit above is what releases the record's references. */
    report_exception_work_unlock(ctx, &s->rep);
    return JS_UNDEFINED;
}

static int js_po_notify_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    PoNotifyTask *s = st;
    int r, k;

    DCHECK(s->hdr.stage == PO_NOTIFY_CALLBACK || s->hdr.stage == PO_NOTIFY_REPORT,
           "§5.3's task resumed into a stage that section does not have");
    if (!s->started) {
        JSValue list;

        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL — the failure path tears the state
           down through `fini` and the visit, which release exactly what the state holds. */
        s->notify = s->cur = s->entry_list = s->cbopts = s->exc = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        report_exception_work_start(&s->rep);
        s->reporting = 0;
        s->started = 1;
        s->i = 0;
        list = po_reg_list(ctx);
        /* STEP 3.1: "Unset performance observer task queued flag of relevantGlobal" — BEFORE the walk, so an
           entry queued by one of these callbacks schedules a NEW task rather than joining the batch being
           delivered. */
        JS_SetProperty(ctx, list, g_atom_queued, JS_FALSE);
        s->notify = po_clone(ctx, list);                                  /* step 3.2 */
        JS_FreeValue(ctx, list);
    }
    for (;;) {
        JSValue ignored = JS_UNDEFINED, cbfn, ostate;

        if (s->reporting) {
            r = report_exception_run(ctx, &s->rep, s->exc, cb_result, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            s->reporting = 0;
            STEP_GOTO(s->hdr.stage, PO_NOTIFY_CALLBACK, &s->phase, NULL);
            JS_FreeValue(ctx, s->exc);
            s->exc = JS_UNDEFINED;
        }
        if (JS_IsUndefined(s->cur)) {
            JSValue rec, po, buffer, entries;
            uint32_t nbuf;

            if (s->i >= po_len(ctx, s->notify)) {
                JS_FreeValue(ctx, s->notify);
                s->notify = JS_UNDEFINED;
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_DONE;
            }
            rec = JS_GetPropertyUint32(ctx, s->notify, s->i++);           /* step 3.3 */
            po = JS_GetPropertyUint32(ctx, rec, PO_R_OBSERVER);           /* step 3.3.1 */
            ostate = po_state(ctx, po);
            DCHECK(!JS_IsException(ostate),
                   "§4's registered-observer list held a record whose observer is not a PerformanceObserver — "
                   "§4.2 steps 6.5 and 7.4 are the only writers of that list and both set it to `this`");
            buffer = JS_GetPropertyUint32(ctx, ostate, PO_S_BUFFER);
            nbuf = po_len(ctx, buffer);
            entries = po_clone(ctx, buffer);                              /* step 3.3.2 */
            JS_FreeValue(ctx, buffer);
            if (nbuf == 0) {
                /* STEP 3.3.3: "If entries is empty, return." PERFORMED LITERALLY, AND THE LITERAL READING IS
                   NOT THE OBVIOUS ONE. This step is a TOP-LEVEL item of step 3.3's substeps, so Infra's
                   "return" terminates the algorithm these substeps ARE — the whole queued task — rather than
                   moving to the next registered observer. An observer standing earlier in the list with an
                   empty buffer therefore ends the delivery for every observer behind it, which is what this
                   code does. It is written this way because the spec is the source of truth in this tree and a
                   browser is confirmation: a `continue` here would be a claim about what other engines do that
                   nothing in this session measured, and CLAUDE.md's §A-FINDING-RELAYED rule is that such a
                   claim must be checked before it is acted on rather than after.
                   WHAT WOULD DECIDE IT, for the next reader who has a browser in front of them: two observers
                   on one document, the first observing a type nothing queues and the second observing `mark`,
                   then one `performance.mark('a')`. If the second observer's callback runs, this step is a
                   spec defect and the repair is to report it upstream and to record the divergence HERE with
                   the evidence — not to change it quietly, which would leave the next reader with a `continue`
                   the standard's own words contradict. */
                JS_FreeValue(ctx, entries);
                JS_FreeValue(ctx, ostate);
                JS_FreeValue(ctx, po);
                JS_FreeValue(ctx, rec);
                JS_FreeValue(ctx, s->notify);
                s->notify = JS_UNDEFINED;
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_DONE;
            }
            /* STEP 3.3.4: "Empty po's observer buffer." A FRESH Array rather than a truncation, because
               `entries` above is a copy that must not move under the callback that is about to read it. */
            JS_SetPropertyUint32(ctx, ostate, PO_S_BUFFER, JS_NewArray(ctx));
            s->entry_list = performance_observer_entry_list_new(ctx, entries);   /* step 3.3.5 */
            JS_FreeValue(ctx, entries);
            /* STEPS 3.3.6 - 3.3.8. `droppedEntriesCount` is left NULL, so §4.1's dictionary has no member at
               all — see performance_observer.h's residual: the count comes from a per-global performance entry
               buffer map this build has none of, and a 0 written here would be a number a page could not tell
               from a measurement. Step 3.3.7.3's "Set po's requires dropped entries to false" is NOT part of
               that residual and runs: it is a fact about the OBSERVER, which this file holds. */
            JS_SetPropertyUint32(ctx, ostate, PO_S_DROPPED, JS_FALSE);           /* step 3.3.7.3 */
            s->cbopts = JS_NewObject(ctx);                                       /* step 3.3.8 */
            CHECK(!JS_IsException(s->cbopts),
                  "§4.1's PerformanceObserverCallbackOptions could not be allocated");
            JS_FreeValue(ctx, ostate);
            JS_FreeValue(ctx, rec);
            s->cur = po;
        }
        /* STEP 3.3.9 — the page's callback, with `this` = po and « observerEntryList, po, callbackOptions ». */
        {
            JSValueConst args[3];

            ostate = po_state(ctx, s->cur);
            DCHECK(!JS_IsException(ostate), "§5.3's walk lost its observer's state mid-callback");
            cbfn = JS_GetPropertyUint32(ctx, ostate, PO_S_CALLBACK);
            JS_FreeValue(ctx, ostate);
            args[0] = s->entry_list;
            args[1] = s->cur;
            args[2] = s->cbopts;
            r = step_call_run(ctx, &s->phase, s->cb, 5, cbfn, s->cur, 3, args, cb_result, &ignored,
                              out_cb, out_argc);
            JS_FreeValue(ctx, cbfn);
            cb_result = JS_UNDEFINED;
            if (r > 0) return JS_STEP_CALL;
        }
        /* "report" is Web IDL §3.12 Invoking callback functions' exception behaviour: the throw is REPORTED and
           the walk goes on. Without it one throwing callback would tear the task down and every observer behind
           it would silently never be notified. */
        if (JS_IsException(ignored)) {
            ignored = JS_UNDEFINED;
            s->exc = JS_GetException(ctx);
            s->reporting = 1;
            STEP_GOTO(s->hdr.stage, PO_NOTIFY_REPORT, &s->phase, NULL);
        }
        JS_FreeValue(ctx, ignored);   /* a PerformanceObserverCallback returns undefined; anything else goes */
        JS_FreeValue(ctx, s->cur);
        JS_FreeValue(ctx, s->entry_list);
        JS_FreeValue(ctx, s->cbopts);
        s->cur = s->entry_list = s->cbopts = JS_UNDEFINED;
    }
}

static const JSTrampStepDef js_po_notify_def = {
    sizeof(PoNotifyTask), js_po_notify_step, js_po_notify_fini, 0,
    /* Step 3.3.9 reports rather than propagates, so the callback's abrupt completion is this machine's own
       VALUE and must be delivered back to step() instead of tearing the task down. */
    .catches_abrupt = 1, .visit = js_po_notify_visit,
    .algorithm = "PERFORMANCE TIMELINE §5.3 queue the PerformanceObserver task",
    .steps = PO_NOTIFY_STEPS
};

/* §5.3 Queue the PerformanceObserver task, given relevantGlobal — which here is `ctx`, the realm the caller is
   running in. */
static void po_queue_task(JSContext *ctx)
{
    JSValue list = po_reg_list(ctx), flag, fn;
    bool queued;

    flag = JS_GetProperty(ctx, list, g_atom_queued);
    queued = JS_ToBool(ctx, flag);
    JS_FreeValue(ctx, flag);
    if (queued) { JS_FreeValue(ctx, list); return; }                      /* step 1 */
    JS_SetProperty(ctx, list, g_atom_queued, JS_TRUE);                    /* step 2 */
    JS_FreeValue(ctx, list);
    /* STEP 3: "Queue a task ... The task source for the queued task is the performance timeline task source."
       The callee is THIS REALM'S — a function object carries the realm it was minted in, and a single static
       one would run §5.3 for every document out of whichever realm built it first. */
    fn = realm_value_get(ctx, g_notify_slot);
    DCHECK(JS_IsFunction(ctx, fn),
           "§5.3's task was queued in a realm that never built its own notification driver");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

/* ---- §5.1 QUEUE A PERFORMANCEENTRY --------------------------------------------------------------------- */

/* §5.1 step 7.1's test over ONE options dictionary: "whose entryTypes member includes entryType or whose type
   member equals to entryType". */
static bool po_options_want(JSContext *ctx, JSValueConst options, JSValueConst entry_type)
{
    JSValue types = idl_dict_get(ctx, options, "entryTypes");
    JSValue type;
    bool want = false;

    if (!JS_IsUndefined(types)) {
        uint32_t n = po_len(ctx, types), i;

        for (i = 0; i < n && !want; i++) {
            JSValue t = JS_GetPropertyUint32(ctx, types, i);

            want = JS_IsStrictEqual(ctx, t, entry_type);
            JS_FreeValue(ctx, t);
        }
    }
    JS_FreeValue(ctx, types);
    if (want) return true;
    type = idl_dict_get(ctx, options, "type");
    want = !JS_IsUndefined(type) && JS_IsStrictEqual(ctx, type, entry_type);
    JS_FreeValue(ctx, type);
    return want;
}

void performance_observer_queue_entry(JSContext *ctx, JSValueConst entry)
{
    PerfEntry *e = performance_entry_of(entry);
    JSValue list, interested;
    uint32_t n, i, k;

    DCHECK(e != NULL,
           "§5.1 Queue a PerformanceEntry was handed something that is not a PerformanceEntry — its callers are "
           "the mints of the timing standards in this build, each of which has just built one");
    /* STEPS 1, 5 and 6 (`id` and `navigationId`) are core/timing/performance_entry.h's residual and are not
       performed here; steps 9-12 (the performance entry buffer) are this file's, named in
       core/timing/performance_observer.h. What is left is the observer half, which is steps 2, 3, 4, 7, 8
       and 13 — the whole of what a page's PerformanceObserver observes. */
    interested = JS_NewArray(ctx);                                        /* step 2 */
    CHECK(!JS_IsException(interested), "§5.1 step 2's interested-observer set could not be allocated");
    /* STEP 3 is `e->entry_type` and STEP 4 is `ctx`: a C member runs in the realm that DEFINED it, and every
       producer in this build mints its entry from a member installed on the same realm the page called it on,
       so the entry's relevant global object is this one. */
    list = po_reg_list(ctx);
    n = po_len(ctx, list);
    for (i = 0; i < n; i++) {                                             /* step 7 */
        JSValue rec = JS_GetPropertyUint32(ctx, list, i);
        JSValue opts = JS_GetPropertyUint32(ctx, rec, PO_R_OPTIONS);
        uint32_t no = po_len(ctx, opts), j;
        bool wants = false;

        for (j = 0; j < no && !wants; j++) {                              /* step 7.1 */
            JSValue o = JS_GetPropertyUint32(ctx, opts, j);

            wants = po_options_want(ctx, o, e->entry_type);
            JS_FreeValue(ctx, o);
        }
        if (wants && po_should_add_entry(ctx, e->entry_type))             /* step 7.1.1 */
            po_push(ctx, interested, JS_GetPropertyUint32(ctx, rec, PO_R_OBSERVER));
        JS_FreeValue(ctx, opts);
        JS_FreeValue(ctx, rec);
    }
    JS_FreeValue(ctx, list);
    n = po_len(ctx, interested);
    for (k = 0; k < n; k++) {                                             /* step 8 */
        JSValue po = JS_GetPropertyUint32(ctx, interested, k);
        JSValue st = po_state(ctx, po);
        JSValue buffer;

        DCHECK(!JS_IsException(st), "§5.1 step 8 met an interested observer that is not a PerformanceObserver");
        buffer = JS_GetPropertyUint32(ctx, st, PO_S_BUFFER);
        po_push(ctx, buffer, JS_DupValue(ctx, entry));                    /* step 8.1 */
        JS_FreeValue(ctx, buffer);
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, po);
    }
    JS_FreeValue(ctx, interested);
    po_queue_task(ctx);                                                   /* step 13 */
}

/* ---- §4.2 observe(), §4.3 takeRecords(), §4.4 disconnect(), §4.5 supportedEntryTypes ---------------------- */

static JSValue js_po_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = po_state(ctx, this_val), list, entry_types, type, buffered, threshold, rec, opts;
    bool et_present, type_present, other_present;
    PoObserverType otype;
    int at;

    (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    /* A DECLARED DICTIONARY POSITION IS CONVERTED EVEN WHEN THE PAGE STOPPED SHORT OF IT, so `observe()`
       arrives here with §4.2.1's object built and every member the page did state already converted. This is
       the ASSERTION of that and not a substitute for it. */
    DCHECK(argc >= 1 && JS_IsObject(argv[0]),
           "observe's options reached the body without the object the dictionary conversion builds — §4's IDL "
           "writes `optional PerformanceObserverInit options = {}` and this body reads it rather than "
           "inventing one");
    /* STEP 1 is `ctx`. NOT ONE MEMBER OF THIS DICTIONARY CARRIES A DEFAULT — §4.2.1's three do not and Event
       Timing API §3.3's partial member does not — so "present" and "omitted" are two states the dictionary
       really carries and the conversion left an absent one absent. That is what steps 2, 3 and 4 branch on,
       and why none of these is read with a `||`. */
    entry_types = idl_dict_get(ctx, argv[0], "entryTypes");
    type        = idl_dict_get(ctx, argv[0], "type");
    buffered    = idl_dict_get(ctx, argv[0], "buffered");
    threshold   = idl_dict_get(ctx, argv[0], "durationThreshold");
    et_present   = !JS_IsUndefined(entry_types);
    type_present = !JS_IsUndefined(type);
    /* STEP 3's "any OTHER member", which is every member of the dictionary that is not `entryTypes` — the two
       §4.2.1 declares beside it and the one Event Timing API §3.3 adds by partial. Listing them is what makes
       the test the standard's rather than a subset of it. */
    other_present = type_present || !JS_IsUndefined(buffered) || !JS_IsUndefined(threshold);
    JS_FreeValue(ctx, buffered);
    JS_FreeValue(ctx, threshold);
    if (!et_present && !type_present) {                                   /* step 2 */
        JS_FreeValue(ctx, entry_types);
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, state);
        return JS_ThrowTypeError(ctx, "PerformanceObserver.observe requires entryTypes or type");
    }
    if (et_present && other_present) {                                    /* step 3 */
        JS_FreeValue(ctx, entry_types);
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, state);
        return JS_ThrowTypeError(ctx, "entryTypes cannot be used together with any other member");
    }
    {                                                                     /* step 4 */
        JSValue cur = JS_GetPropertyUint32(ctx, state, PO_S_TYPE);
        int32_t t = PO_TYPE_UNDEFINED;

        JS_ToInt32(ctx, &t, cur);
        JS_FreeValue(ctx, cur);
        otype = (PoObserverType)t;
        /* STEPS 4.1.1 AND 4.1.2 ARE ONE TERNARY BECAUSE STEPS 2 AND 3 HAVE ALREADY RUN: neither present is
           the TypeError step 2 threw, and both present is the one step 3 threw (`type` is one of the "other"
           members `entryTypes` may not stand beside), so exactly one of the two is present here. */
        if (otype == PO_TYPE_UNDEFINED)                                   /* step 4.1 */
            otype = et_present ? PO_TYPE_MULTIPLE : PO_TYPE_SINGLE;       /* steps 4.1.1 and 4.1.2 */
        if ((otype == PO_TYPE_SINGLE && et_present) ||                    /* step 4.2 */
            (otype == PO_TYPE_MULTIPLE && type_present)) {                /* step 4.3 */
            JS_FreeValue(ctx, entry_types);
            JS_FreeValue(ctx, type);
            JS_FreeValue(ctx, state);
            return JS_ThrowDOMException(ctx, "InvalidModificationError",
                                        "this observer was already observing with the other of "
                                        "PerformanceObserverInit's two selectors");
        }
        JS_SetPropertyUint32(ctx, state, PO_S_TYPE, JS_NewInt32(ctx, (int32_t)otype));
    }
    JS_SetPropertyUint32(ctx, state, PO_S_DROPPED, JS_TRUE);              /* step 5 */
    list = po_reg_list(ctx);
    at = po_registration_of(ctx, list, this_val);
    if (otype == PO_TYPE_MULTIPLE) {                                      /* step 6 */
        JSValue kept = JS_NewArray(ctx);                                  /* steps 6.1 and 6.2 */
        uint32_t n = po_len(ctx, entry_types), i;

        CHECK(!JS_IsException(kept), "§4.2 step 6.1's entry types sequence could not be allocated");
        for (i = 0; i < n; i++) {
            JSValue t = JS_GetPropertyUint32(ctx, entry_types, i);

            if (po_type_supported(ctx, t)) po_push(ctx, kept, t);
            else JS_FreeValue(ctx, t);
        }
        if (po_len(ctx, kept) == 0) {                                     /* step 6.3: abort these steps */
            JS_FreeValue(ctx, kept);
            JS_FreeValue(ctx, list);
            JS_FreeValue(ctx, entry_types);
            JS_FreeValue(ctx, type);
            JS_FreeValue(ctx, state);
            return JS_UNDEFINED;
        }
        /* STEP 6.2 REMOVES THE UNSUPPORTED TYPES FROM THE SEQUENCE, and steps 6.4/6.5 then store `options` —
           the SAME dictionary, whose `entryTypes` is the sequence step 6.2 just narrowed. So the narrowed list
           is written back onto the dictionary rather than kept beside it: §5.1 step 7.1 reads `entryTypes` off
           the stored options, and a copy left unnarrowed there would make an observer interested in a type
           §4.5 says this build cannot mint. The dictionary is the CONVERSION's own object, not the page's, so
           this write is not observable to script. */
        JS_SetPropertyStr(ctx, (JSValue)argv[0], "entryTypes", kept);
        opts = JS_NewArray(ctx);
        CHECK(!JS_IsException(opts), "§4.2 step 6.4's options list could not be allocated");
        po_push(ctx, opts, JS_DupValue(ctx, argv[0]));
        if (at >= 0) {                                                    /* step 6.4 */
            JSValue r = JS_GetPropertyUint32(ctx, list, (uint32_t)at);

            JS_SetPropertyUint32(ctx, r, PO_R_OPTIONS, opts);
            JS_FreeValue(ctx, r);
        } else {                                                          /* step 6.5 */
            rec = JS_NewArray(ctx);
            CHECK(!JS_IsException(rec), "§4.2 step 6.5's registered performance observer could not be allocated");
            JS_SetPropertyUint32(ctx, rec, PO_R_OBSERVER, JS_DupValue(ctx, this_val));
            JS_SetPropertyUint32(ctx, rec, PO_R_OPTIONS, opts);
            po_push(ctx, list, rec);
        }
    } else {                                                              /* step 7 */
        DCHECK(otype == PO_TYPE_SINGLE, "§4.2 step 7.1's assertion: the observer type is \"single\" here");
        if (!po_type_supported(ctx, type)) {                              /* step 7.2: abort these steps */
            JS_FreeValue(ctx, list);
            JS_FreeValue(ctx, entry_types);
            JS_FreeValue(ctx, type);
            JS_FreeValue(ctx, state);
            return JS_UNDEFINED;
        }
        if (at >= 0) {                                                    /* step 7.3 */
            JSValue r = JS_GetPropertyUint32(ctx, list, (uint32_t)at);
            uint32_t n, j;
            int found = -1;

            opts = JS_GetPropertyUint32(ctx, r, PO_R_OPTIONS);
            n = po_len(ctx, opts);
            for (j = 0; j < n && found < 0; j++) {                        /* step 7.3.1 */
                JSValue o = JS_GetPropertyUint32(ctx, opts, j);
                JSValue ot = idl_dict_get(ctx, o, "type");

                if (!JS_IsUndefined(ot) && JS_IsStrictEqual(ctx, ot, type)) found = (int)j;
                JS_FreeValue(ctx, ot);
                JS_FreeValue(ctx, o);
            }
            if (found >= 0) JS_SetPropertyUint32(ctx, opts, (uint32_t)found, JS_DupValue(ctx, argv[0]));
            else po_push(ctx, opts, JS_DupValue(ctx, argv[0]));           /* step 7.3.2 */
            JS_FreeValue(ctx, opts);
            JS_FreeValue(ctx, r);
        } else {                                                          /* step 7.4 */
            opts = JS_NewArray(ctx);
            CHECK(!JS_IsException(opts), "§4.2 step 7.4's options list could not be allocated");
            po_push(ctx, opts, JS_DupValue(ctx, argv[0]));
            rec = JS_NewArray(ctx);
            CHECK(!JS_IsException(rec), "§4.2 step 7.4's registered performance observer could not be allocated");
            JS_SetPropertyUint32(ctx, rec, PO_R_OBSERVER, JS_DupValue(ctx, this_val));
            JS_SetPropertyUint32(ctx, rec, PO_R_OPTIONS, opts);
            po_push(ctx, list, rec);
        }
        /* STEP 7.5's `buffered` arm is performance_observer.h's NAMED RESIDUAL: there is no per-global
           performance entry buffer map in this build, so the historical entries it would deliver do not exist
           and there is nothing here to read. The flag is still CONVERTED and still refused by step 3 beside
           `entryTypes`, which is the half of it that is observable today. */
    }
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, entry_types);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* §4.3: "The takeRecords() method must return a copy of this's observer buffer, and also empty this's observer
   buffer." A `PerformanceEntryList`, which Web IDL renders as a JS Array. */
static JSValue js_po_take(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = po_state(ctx, this_val), buffer, out;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    buffer = JS_GetPropertyUint32(ctx, state, PO_S_BUFFER);
    out = po_clone(ctx, buffer);
    JS_FreeValue(ctx, buffer);
    /* A FRESH Array rather than a truncation of the one just copied — the copy above and the emptied buffer
       must be two objects, or a page that keeps the returned list watches it empty itself. */
    JS_SetPropertyUint32(ctx, state, PO_S_BUFFER, JS_NewArray(ctx));
    JS_FreeValue(ctx, state);
    return out;
}

static JSValue js_po_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = po_state(ctx, this_val), list;
    int at;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    list = po_reg_list(ctx);
    at = po_registration_of(ctx, list, this_val);
    if (at >= 0) {                                                        /* step 1 */
        uint32_t n = po_len(ctx, list), i;

        for (i = (uint32_t)at + 1; i < n; i++)
            JS_SetPropertyUint32(ctx, list, i - 1, JS_GetPropertyUint32(ctx, list, i));
        JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n - 1));
    }
    JS_FreeValue(ctx, list);
    /* STEP 2: "Empty this's observer buffer." */
    JS_SetPropertyUint32(ctx, state, PO_S_BUFFER, JS_NewArray(ctx));
    /* STEP 3: "Empty this's options list." §4 puts the options list on the REGISTERED PERFORMANCE OBSERVER
       struct and not on the observer — §4.2 steps 6.4/6.5 and 7.3/7.4 are its only writers and each names
       `regObs`'s — so step 1 above, which removes that struct, is what empties it. There is no second copy
       here to clear, and adding one so that this step had its own line would be two places holding one list. */
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* PERFORMANCE TIMELINE §4.5 "supportedEntryTypes attribute": "Let globalObject be the environment settings
   object's global object. Return globalObject's frozen array of supported entry types." A C member runs in
   the realm that defined it, so
   `ctx` IS that global object's realm and the array is this realm's — [SameObject] holds because the array is
   built once with the realm and every call hands back that same object. */
static JSValue js_po_supported(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue types = realm_value_get(ctx, g_types_slot);

    (void)this_val; (void)magic;
    DCHECK(JS_IsArray(types),
           "§4.5's getter answered out of a realm that never built its own array — it is built EAGERLY with "
           "the realm, so an absence here is an install column that did not run rather than a page reading "
           "too early");
    return types;
}

/* ---- §4's CONSTRUCTOR ------------------------------------------------------------------------------------ */

typedef struct { uint8_t unused; } JSPoCtorState;
static void js_po_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define PO_CTOR_STAGES(X)                                                                                     \
    X(PO_CTOR_BUILD = IDL_STEP_FIRST,                                                                         \
      "PERFORMANCE TIMELINE §4 \"The PerformanceObserver interface\" new PerformanceObserver(callback) "     \
      "(\"create a new PerformanceObserver object "                                                            \
      "with its observer callback set to callback and then return it\", with §4's other three concepts at "    \
      "the initial values that section states)")
enum { PO_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PO_CTOR_STEPS[] = { PO_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_po_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, state, proto;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == PO_CTOR_BUILD,
           "the PerformanceObserver constructor resumed at a stage §4 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor PerformanceObserver requires 'new'"), -1;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PerformanceObserver requires a callback"), -1;
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "PerformanceObserver's callback reached the body unconverted — §4 declares it a "
           "PerformanceObserverCallback, and Web IDL §3.2.19 Callback function types' brand test is what makes "
           "a non-callable a TypeError before the object is built");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "a PerformanceObserver was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return -1;
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "a PerformanceObserver's state could not be allocated");
    /* §4's FOUR ASSOCIATED CONCEPTS, every one of them written here — "a PerformanceObserverCallback observer
       callback set on creation", "a PerformanceEntryList object called the observer buffer that is initially
       empty", "a DOMString observer type which is initially "undefined"", and "a boolean requires dropped
       entries which is initially set to false". Written at construction so no reader anywhere has an absence
       to default past. */
    JS_SetPropertyUint32(ctx, state, PO_S_CALLBACK, JS_DupValue(ctx, argv[0]));
    JS_SetPropertyUint32(ctx, state, PO_S_BUFFER, JS_NewArray(ctx));
    JS_SetPropertyUint32(ctx, state, PO_S_TYPE, JS_NewInt32(ctx, (int32_t)PO_TYPE_UNDEFINED));
    JS_SetPropertyUint32(ctx, state, PO_S_DROPPED, JS_FALSE);
    JS_DefinePropertyValue(ctx, obj, g_atom_state, state, 0);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_po_ctor_decl = {
    js_po_ctor_step, sizeof(JSPoCtorState), js_po_ctor_visit, NULL,
    "PERFORMANCE TIMELINE §4 \"The PerformanceObserver interface\" new PerformanceObserver(callback)",
    PO_CTOR_STEPS
};

/* ---- declaration and installation ------------------------------------------------------------------------- */

void performance_observer_init(JSContext *ctx)
{
    /* §4.2.1's `dictionary PerformanceObserverInit`, MEMBER FOR MEMBER — in Web IDL §3.2.17's sorted member
       list order, which is what every other dictionary declaration in this tree is written in. None of the
       three carries a default, so an absent one stays absent and steps 2, 3 and 4 can tell "omitted" from
       "present and false" — which is exactly what IDL_BOOLEAN_NO_DEFAULT exists for. */
    static const IdlDictMember OBSERVE_INIT[] = {
        { "buffered",          IDL_BOOLEAN_NO_DEFAULT, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        /* NOT §4.2.1's OWN MEMBER, AND IT IS DECLARED ANYWAY. Event Timing API §3.3 "Modifications to the
           Performance Timeline specification" writes `partial dictionary PerformanceObserverInit {
           DOMHighResTimeStamp durationThreshold; }`, and Web IDL §3.2.17 converts a dictionary WITH its
           partials — so this member exists on every PerformanceObserverInit a page passes. It is not
           decoration here: §4.2 step 3 refuses `entryTypes` beside "any other member", and a member left
           undeclared is one the conversion never places, so `{entryTypes:['mark'], durationThreshold:40}`
           would have been ACCEPTED where the standard throws. Nothing in this build READS it — the entry type
           it filters (`event`) has no producer — which is why it appears in step 3's test and nowhere else. */
        { "durationThreshold", IDL_DOUBLE,             false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "entryTypes",        IDL_SEQUENCE_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "type",              IDL_DOMSTRING,          false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    };
    static const IdlArgType OBSERVE_ARGS[1] = { IDL_DICT };
    static const IdlArgType CTOR_ARGS[1] = { IDL_CALLBACK };
    JSClassDef d = { "PerformanceObserver" };

    /* NOT `if (g_ready) return;` — this component has exactly ONE declaration site, core/platform.c's row, so
       the test could never be true and what it could do is hide a release that left the latch set. */
    DCHECK(!g_ready, "performance_observer_init ran twice — §4's class is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    performance_observer_entry_list_init(ctx);

    g_state_key = JS_NewSymbol(ctx, "performanceObserverState", false);
    CHECK(!JS_IsException(g_state_key), "the PerformanceObserver state slot key could not be allocated");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    g_atom_queued = JS_NewAtom(ctx, "queued");
    CHECK(g_atom_state != JS_ATOM_NULL && g_atom_queued != JS_ATOM_NULL,
          "a Performance Timeline slot key could not be interned");

    g_notify_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_po_notify_def);
    CHECK(g_notify_stepid >= 0, "no step id for §5.3's task machine");
    g_reg_slot    = realm_value_declare(ctx, "§2's list of registered performance observer objects");
    g_notify_slot = realm_value_declare(ctx, "§5.3's queued task callee");
    g_types_slot  = realm_value_declare(ctx, "§4.5's frozen array of supported entry types");

    g_id_ctor = idl_method_id_step(ctx, CTOR_ARGS, 1, NULL, 0, &js_po_ctor_decl, 0);
    g_id_observe = idl_method_id_dict(ctx, OBSERVE_ARGS, 1, OBSERVE_INIT, (int)COUNTOF(OBSERVE_INIT),
                                      js_po_observe, 0);
    idl_optional_from(0);                /* `optional PerformanceObserverInit options = {}` */
    g_id_take = idl_method_id(ctx, NULL, 0, js_po_take, 0);
    g_id_disconnect = idl_method_id(ctx, NULL, 0, js_po_disconnect, 0);

    realm_declare_intrinsic(performance_observer_install_protos);
    g_ready = 1;

    agent_state_flag("performance_observer", &g_ready, "the declaration latch");
    agent_state_class("performance_observer", &g_class, "§4's PerformanceObserver class");
    agent_state_value("performance_observer", &g_state_key, "the observer's state-slot key");
    agent_state_atom("performance_observer", &g_atom_state, "the observer's state-slot key, interned");
    agent_state_atom("performance_observer", &g_atom_queued,
                     "§5.3's performance observer task queued flag's field name");
    agent_state_id("performance_observer", &g_reg_slot,
                   "the per-realm slot §2's list of registered performance observer objects is held in");
    agent_state_id("performance_observer", &g_notify_slot, "the per-realm slot §5.3's callee is held in");
    agent_state_id("performance_observer", &g_types_slot, "the per-realm slot §4.5's frozen array is held in");
    agent_state_id("performance_observer", &g_notify_stepid, "§5.3's task machine");
    agent_state_id("performance_observer", &g_id_ctor, "§4's constructor declaration");
    agent_state_id("performance_observer", &g_id_observe, "§4.2's observe declaration");
    agent_state_id("performance_observer", &g_id_take, "§4.3's takeRecords declaration");
    agent_state_id("performance_observer", &g_id_disconnect, "§4.4's disconnect declaration");
}

void performance_observer_install_protos(JSContext *ctx)
{
    JSValue proto, prev, list, types;
    int i, j;

    DCHECK(g_class != 0, "a realm asked for PerformanceObserver.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "performance_observer_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    performance_observer_entry_list_install(ctx);

    /* §4's per-GLOBAL state, built EAGERLY with the realm — the registered-observer list, and riding it §5.3's
       task queued flag. Built lazily on first `observe` it would be created inside whichever flow happened to
       observe first, which puts a baseline object in one flow's COW delta and makes every sibling's observer
       list that flow's. */
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a realm's §4 registered-observer list could not be allocated");
    JS_SetProperty(ctx, list, g_atom_queued, JS_FALSE);
    realm_value_set(ctx, g_reg_slot, list);
    {
        /* §5.3's TASK CALLEE IS THIS REALM'S — see po_queue_task. It is a step function object nobody
           installs, so a page can neither see it nor replace it. */
        JSValue fn = JS_NewCFunction2(ctx, NULL, "queuePerformanceObserverTask", 0, JS_CFUNC_step,
                                      g_notify_stepid);

        CHECK(!JS_IsException(fn), "§5.3's task callee could not be allocated");
        realm_value_set(ctx, g_notify_slot, fn);
    }
    /* §4.5's "frozen array of supported entry types ... created from the sequence of strings among the registry
       that are supported for the global object in ALPHABETICAL ORDER". The membership is the producers' (see
       performance_observer_declare_entry_type); the order is this line's, by insertion over a set this small. */
    types = JS_NewArray(ctx);
    CHECK(!JS_IsException(types), "§4.5's frozen array could not be allocated");
    for (i = 0; i < g_n_entry_types; i++) {
        for (j = i; j > 0; j--) {
            JSValue prev_s = JS_GetPropertyUint32(ctx, types, (uint32_t)(j - 1));
            const char *p = JS_ToCString(ctx, prev_s);
            bool after;

            CHECK(p != NULL, "§4.5's array could not read back a name it had just written");
            after = strcmp(p, g_entry_types[i]) <= 0;
            JS_FreeCString(ctx, p);
            JS_FreeValue(ctx, prev_s);
            if (after) break;
            JS_SetPropertyUint32(ctx, types, (uint32_t)j,
                                 JS_GetPropertyUint32(ctx, types, (uint32_t)(j - 1)));
        }
        JS_SetPropertyUint32(ctx, types, (uint32_t)j, JS_NewString(ctx, g_entry_types[i]));
    }
    CHECK(idl_freeze_array(ctx, types) >= 0, "§4.5's array could not be frozen");
    realm_value_set(ctx, g_types_slot, types);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "PerformanceObserver.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PerformanceObserver");
    idl_install_method(ctx, proto, "observe", g_id_observe);
    idl_install_method(ctx, proto, "disconnect", g_id_disconnect);
    idl_install_method(ctx, proto, "takeRecords", g_id_take);
    JS_SetClassProto(ctx, g_class, proto);
}

void performance_observer_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_id_ctor >= 0, "PerformanceObserver was installed before performance_observer_init declared it");
    ctor = idl_step_constructor(ctx, "PerformanceObserver", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the PerformanceObserver interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "PerformanceObserver was installed in a realm that never ran its proto build");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    /* §4's `[SameObject] static readonly attribute FrozenArray<DOMString> supportedEntryTypes` — a STATIC
       attribute, so §3.7.6 puts it on the INTERFACE OBJECT rather than on the prototype. */
    idl_install_accessor(ctx, ctor, "supportedEntryTypes", js_po_supported, 0, -1);
    idl_define_global_property_reference(ctx, global, "PerformanceObserver", ctor);
}

void performance_observer_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — the declare pass this pairs with is unconditional, so a release reached in
       an agent that never declared is the thing to CRASH on rather than the thing to skip. */
    DCHECK(g_ready, "§4 was released in an agent that never declared it");
    performance_observer_entry_list_free(rt);
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_state);
    JS_FreeAtomRT(rt, g_atom_queued);
    g_atom_state = g_atom_queued = JS_ATOM_NULL;
    /* The per-realm list, callee and frozen array are the REALMS' and go with their contexts; what is agent
       state about them is the SLOT NUMBER. The entry-type DECLARATIONS are the agent's too: a new agent's
       producers declare them again from their own `_init`s. */
    g_reg_slot = g_notify_slot = g_types_slot = -1;
    g_notify_stepid = -1;
    g_id_ctor = g_id_observe = g_id_take = g_id_disconnect = -1;
    g_n_entry_types = 0;
    g_class = 0;
    g_ready = 0;
}
