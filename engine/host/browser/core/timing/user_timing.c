/* USER TIMING §2 and §3 — §2.1.1 mark(), §2.1.3 measure(), §2.2 The PerformanceMark Interface, §2.2.1 The
   PerformanceMark Constructor, §2.3 The PerformanceMeasure Interface, and §3.1/§3.2's two conversions.
   See user_timing.h for what is built, what is a named residual, and why.

   EVERY QUOTATION IN THIS FILE WAS PASTED FROM THE FETCHED EDITOR'S DRAFT, AND NOTHING IN THIS TREE CAN CHECK
   THAT. engine/citegen.mjs compares a quotation against the cited section's committed text, and the committed
   index for this standard carries section TITLES ONLY — so its title channel judges every citation here and
   its quotation channel judges none of them. That is a property of the corpus rather than of the tool, it is
   checkable in one command, and it is written here because a reader would otherwise read this file's
   quotations as verified by the same instrument that verifies its numbers:
     node -e 'const s=require("./engine/specindex/usertiming.json").sections;
              console.log(Object.values(s).filter(v=>v.page&&v.page.length).length)'
   An answer of 0 means what it says now; a positive answer means the quotation channel has become the check
   and this paragraph is spent. The standing obligation the absence leaves is the one CLAUDE.md already puts
   on every citation — paste from a fetch of the base the specindex row records, never from memory — and the
   stamps were equal when these landed (User Timing and Navigation Timing 1 September 2026, Performance
   Timeline 24 March 2026), which is what makes the paste safe rather than merely current.
   RETIREMENT: this paragraph goes when that command answers non-zero. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>      /* INFINITY — the registry's maxBufferSize for both declared types */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_name_chain.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/frame/window.h"
#include "core/timing/performance.h"
#include "core/timing/performance_entry.h"
#include "core/timing/performance_observer.h"
#include "core/timing/user_timing.h"
#include "solver/concolic.h"   /* an unknown NAME denotes its SHAPE — see concolic_name_cstr */
#include "solver/cow.h"

/* §2.2's ONE ADDED FIELD over PERFORMANCE TIMELINE §3's four. The base is the FIRST member, which is what
   makes the one class id and the one opaque cover both halves — see performance_entry.h's PerfEntryClass. */
typedef struct {
    PerfEntry base;
    JSValue   detail;   /* §2.2's detail — OWNED */
} PerfMark;

/* THE ONE STATEMENT OF WHAT THE WHOLE RECORD OWNS: §3's three values and §2.2's fourth, which is why the base
   offsets appear here rather than in performance_entry.c. The capture dups every entry in this list, so a list
   that stopped at the base would leave `detail` un-dup'd in a delta that restores it. */
static const uint16_t MARK_VALS[] = { (uint16_t)offsetof(PerfMark, base.name),
                                      (uint16_t)offsetof(PerfMark, base.entry_type),
                                      (uint16_t)offsetof(PerfMark, base.start_time),
                                      (uint16_t)offsetof(PerfMark, detail) };
static const CowRecord MARK_REC = { sizeof(PerfMark), MARK_VALS, 4 };

/* THE DERIVED HALF OF THE FINALIZER AND THE gc_mark — §2.2's field ONLY. §3's three are freed and marked by
   the component that owns them; a copy here would double-free them. */
static void mark_release(JSRuntime *rt, void *derived)
{
    JS_FreeValueRT(rt, ((PerfMark *)derived)->detail);
}

static void mark_gc_mark(JSRuntime *rt, void *derived, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, ((PerfMark *)derived)->detail, mark_func);
}

/* THE BRAND, and it is a STATIC ADDRESS rather than a class id for the reason performance_entry.h gives: every
   entry in the platform wears §3's one class, so `JS_GetClassID` cannot tell a mark from a measure, and this
   pointer is written by exactly one line of this file and cannot be forged from a page. */
static const PerfEntryClass MARK_CLASS = { "PerformanceMark", &MARK_REC, mark_release, mark_gc_mark };

static JSClassID g_mark_proto_slot;   /* a per-realm PROTOTYPE SLOT — NOT the class an instance wears */
static int g_ctor_stepid = -1;        /* §2.2.1's constructor machine */
static int g_id_mark = -1;            /* §2.1.1's mark() machine */

bool performance_mark_is(JSValueConst v)
{
    return performance_entry_is_a(v, &MARK_CLASS);
}

/* USER TIMING §2.2: "The detail attribute must return the value it is set to (it's copied from the
   PerformanceMarkOptions dictionary)." The copy is §2.2.1 steps 8.1 and 8.2's serialize-then-deserialize and
   has already happened, so
   this hands back the same object on every read — which is what a page comparing `m.detail === m.detail`
   observes in a browser.
   WEB IDL §3.7.6 Attributes' BRAND is stated here because a plain-C getter has nowhere else to put it and an
   attribute's getter has nothing to convert, so there is no ordering hazard. A real TypeError and not an
   assert: a feature detector that applies the getter to a bare object reads the throw as "this is a real
   interface", and tells it apart from `undefined`. */
static JSValue js_mark_get_detail(JSContext *ctx, JSValueConst this_val, int magic)
{
    PerfEntry *e;

    (void)magic;
    if (!performance_mark_is(this_val))
        return JS_ThrowTypeError(ctx, "PerformanceMark.detail was reached on something that is not a "
                                      "PerformanceMark");
    e = performance_entry_of(this_val);   /* through the base's accessor, which is where the capture lives */
    DCHECK(e != NULL, "a value that passed §2.2's brand answered no record — the brand IS a read of the record, "
                      "so the two cannot disagree unless the entry was released between them");
    return JS_DupValue(ctx, ((PerfMark *)e)->detail);
}

/* ---- §2.2.1 The PerformanceMark Constructor ----------------------------------------------------------------
 *
 * NAVIGATION TIMING §8.1 The PerformanceTiming interface's READ ONLY ATTRIBUTES — the twenty-one names step 1
 * refuses. They are §8.1's IDL in its own order, and every one is `readonly attribute unsigned long long`;
 * `toJSON` is on that interface too and is NOT here, because step 1 says "a read only attribute" and toJSON is
 * an operation. The interface is in that standard's §8 Obsolete section and this engine does not implement it,
 * which changes nothing: step 1 asks about the NAMES the interface declares, so the list is a fact about the
 * standard rather than about what this build exposes.
 * THIS IS THE ONE PLACE A NAME IS MATCHED, and it is matching in the REFUSING direction — it asserts no value
 * and computes none, it only declines to mint an entry the standard says must not exist. */
static const char *const PERFORMANCE_TIMING_ATTRS[] = {
    "navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd", "fetchStart",
    "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart",
    "requestStart", "responseStart", "responseEnd", "domLoading", "domInteractive",
    "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete", "loadEventStart", "loadEventEnd",
};

/* Step 1: "If the current global object is a Window object and markName uses the same name as a read only
   attribute in the PerformanceTiming interface, throw a SyntaxError."
   AN UNKNOWN NAME DENOTES ITS SHAPE, which is this engine's standing answer for a name it did not compute
   (concolic_name_cstr — the same accessor a selector, an attribute name and a channel name already ask). It is
   the right answer here and not merely a tolerable one: a shape is a real, stable string, and step 1 is an
   EQUALITY against twenty-one fixed identifiers that no shape spells, so `performance.mark(location.hash)`
   takes the arm the standard takes for every name that is not one of the twenty-one. NOTHING FORKS, because
   the refused arm reaches no code at all — it throws — so a fork here would mint a sibling world whose entire
   future is one exception. Returns -1 with the throw live, 0 to continue. */
static int mark_check_name(JSContext *ctx, JSValueConst mark_name)
{
    JSValue global;
    const char *n;
    size_t i;
    bool is_window, refuse = false;

    global = JS_GetGlobalObject(ctx);
    is_window = window_is(global);
    JS_FreeValue(ctx, global);
    if (!is_window) return 0;
    n = concolic_name_cstr(ctx, mark_name);
    if (!n) return -1;
    for (i = 0; i < sizeof PERFORMANCE_TIMING_ATTRS / sizeof PERFORMANCE_TIMING_ATTRS[0]; i++)
        if (strcmp(n, PERFORMANCE_TIMING_ATTRS[i]) == 0) { refuse = true; break; }
    JS_FreeCString(ctx, n);
    if (!refuse) return 0;
    JS_ThrowDOMException(ctx, "SyntaxError",
                         "a mark may not be named after a read only attribute of the PerformanceTiming "
                         "interface");
    return -1;
}

/* §2.2.1's EIGHT STEPS, as one operation, because USER TIMING §2.1.1 step 1 is literally "Run the
   PerformanceMark constructor and let entry be the newly created object" — so the member and the constructor
   are not two algorithms that happen to agree, they are one algorithm with two doors. Two copies would be the
   dual system this codebase forbids, and the seam between them is where the drift would be.
   `mark_options` is the dictionary the DECLARATION converted, or JS_UNDEFINED where the page passed nothing —
   idl_dict_get answers every member of an absent dictionary as absent, which is the whole of what `optional
   PerformanceMarkOptions markOptions = {}` means here. */
static JSValue mark_construct(JSContext *ctx, JSValueConst mark_name, JSValueConst mark_options)
{
    JSValue proto, obj, detail, start_time, entry_type;
    PerfMark *m;

    /* STEP 1. */
    if (mark_check_name(ctx, mark_name) < 0) return JS_EXCEPTION;

    /* STEP 5: "Set entry's startTime attribute as follows". Read before the object exists because step 5.1.1
       can THROW, and a partially-built entry would have to be torn down. */
    start_time = idl_dict_get(ctx, mark_options, "startTime");
    if (JS_IsUndefined(start_time)) {
        /* STEP 5.2: "Otherwise, set it to the value that would be returned by the Performance object's now()
           method." Not a second clock: HIGH RESOLUTION TIME §7.1's own answer, through the component that owns
           the Performance object, so a mark and a `performance.now()` on the same line agree by construction. */
        JS_FreeValue(ctx, start_time);
        start_time = performance_now_value(ctx);
        if (JS_IsException(start_time)) return JS_EXCEPTION;
    } else {
        double d = 0;

        IDL_DCHECK_MEMBER(JS_IsNumber(start_time) || concolic_is(start_time), start_time, "startTime",
                          "DOMHighResTimeStamp, which is a `double`");
        /* STEP 5.1.1: "If markOptions's startTime is negative, throw a TypeError."
           THE PREDICATE IS RUN ON A REAL NUMBER OR IT IS NOT RUN, and which of those happens is a fact about
           the value rather than a policy: idl_number_of answers the number an unknown DENOTES — the real
           §3.2 conversion over the example the code actually computed — and answers 0 when the unknown carries
           no example yet. That 0 is a POSITIVE statement ("there is no number here"), and the caller owes it
           an answer; the answer HERE is that an unknown with no example has not been observed to be negative,
           so step 5.1.2 applies and the entry's startTime is the unknown ITSELF. Both halves of that matter:
           the refused arm throws and therefore reaches no code, so declining to take it costs the search
           nothing, and keeping the value unknown is what lets a page's `sink(m.startTime)` still carry it. */
        if (idl_number_of(ctx, IDL_DOUBLE, start_time, &d) && d < 0) {
            JS_FreeValue(ctx, start_time);
            return JS_ThrowTypeError(ctx, "a mark's startTime may not be negative");
        }
        /* STEP 5.1.2 — the value is kept as it arrived. */
    }

    /* STEPS 7 AND 8: "If markOptions's detail is null, set entry's detail to null." / "Otherwise: let record be
       the result of calling the StructuredSerialize algorithm on markOptions's detail; set entry's detail to
       the result of calling the StructuredDeserialize algorithm on record and the current realm."
       AN ABSENT `detail` IS NEITHER OF THOSE TWO ARMS, AND THAT IS A GAP IN THE STANDARD RATHER THAN A CHOICE
       THIS FILE IS MAKING QUIETLY. §2.1.1.1 declares `any detail;` with NO default value, and Web IDL §2.7
       Dictionaries is explicit about what that means — "Other members' entries might or might not exist in
       the dictionary value ... a value of undefined for the property corresponding to a dictionary member is
       treated the same as omitting that property ... will result in no entry existing in the dictionary value"
       — so for `performance.mark('a')` the member does not exist, step 7's test is not met, and step 8 has no
       operand to serialize. The only reading under which §2.2.1 terminates is that an absent detail takes step
       7's arm, which is also what browsers answer (`performance.mark('a').detail` is null). Written here as
       the one arm rather than left to a `||`, because a default filled at a READ is the shape that cannot be
       told from a measurement.
       StructuredSerialize REFUSES a function, a Proxy, a Promise or a platform object with a "DataCloneError"
       DOMException, and that throw is the page's to see — §2.2.1 states no catch. */
    detail = idl_dict_get(ctx, mark_options, "detail");
    if (JS_IsUndefined(detail) || JS_IsNull(detail)) {
        JS_FreeValue(ctx, detail);
        detail = JS_NULL;
    } else {
        JSValue copy = structured_clone(ctx, detail);

        JS_FreeValue(ctx, detail);
        if (JS_IsException(copy)) { JS_FreeValue(ctx, start_time); return JS_EXCEPTION; }
        detail = copy;
    }

    /* STEP 2: "Create a new PerformanceMark object (entry) with the current global object's realm." `ctx` IS
       that realm — a C member runs in the realm that DEFINED it, and both doors onto this operation are
       per-realm objects this component installed, so the prototype below is THIS document's. */
    proto = JS_GetClassProto(ctx, g_mark_proto_slot);
    DCHECK(!JS_IsNull(proto), "a PerformanceMark was constructed in a realm that never ran its install — the "
                              "two doors onto this operation are both members installed by that same install, "
                              "so reaching here without it means the prototype was taken from another realm");
    m = calloc(1, sizeof *m);
    CHECK(m != NULL, "user timing: a PerformanceMark's record could not be allocated");
    /* STEP 4: "Set entry's entryType attribute to DOMString "mark"." A constant of this standard, which is why
       PERFORMANCE TIMELINE §3's own step 1 can assert it is in the Timing Entry Types Registry. */
    entry_type = JS_NewString(ctx, "mark");
    if (JS_IsException(entry_type)) {
        free(m);
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, detail);
        JS_FreeValue(ctx, start_time);
        return JS_EXCEPTION;
    }
    m->detail = detail;
    /* STEPS 3, 5 and 6 are PERFORMANCE TIMELINE §3's "initialize a PerformanceEntry" over this record — step
       3 is its name, step 5 its startTime, and USER TIMING §2.2.1 STEP 6 ("Set entry's duration attribute to
       0") is its endTime, which PERFORMANCE TIMELINE §3 defaults to 0 and whose getter then answers 0 without
       this file stating the number a second time. */
    obj = performance_entry_new(ctx, proto, &MARK_CLASS, m, start_time, entry_type,
                                JS_DupValue(ctx, mark_name), 0);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { mark_release(JS_GetRuntime(ctx), m); free(m); return JS_EXCEPTION; }
    return obj;
}

/* ---- the two doors ----------------------------------------------------------------------------------------
 *
 * BOTH ARE STEP MACHINES BECAUSE BOTH ARE CONSTRUCTORS' SHAPE, not because either body suspends: Web IDL
 * declares a constructor through idl_step_constructor, which takes a step id, and §2.1.1's member is declared
 * the same way so the two share one argument list and one set of stage labels' discipline. Neither body
 * reaches the page's code — the declaration converted `markName` and both dictionary members before either
 * ran, and structured_clone is a C walk — so each has exactly one stage and never returns to it. */
typedef struct { uint8_t unused; } JSMarkCtorState;
static void js_mark_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define UT_CTOR_STAGES(X) \
    X(UT_CTOR_BUILD = IDL_STEP_FIRST, \
      "USER TIMING §2.2.1 The PerformanceMark Constructor steps 1-8 (the name check, the object, its four §3 " \
      "fields, and the structured copy of `detail`)")
enum { UT_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UT_CTOR_STEPS[] = { UT_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

#define UT_MARK_STAGES(X) \
    X(UT_MARK_BUILD = IDL_STEP_FIRST, \
      "USER TIMING §2.1.1 mark() steps 1, 2 and 4 (run the PerformanceMark constructor, queue a " \
      "PerformanceEntry, and return the entry); step 3's performance entry buffer is user_timing.h's " \
      "named residual")
enum { UT_MARK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UT_MARK_STEPS[] = { UT_MARK_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §2.2.1's door. `constructor(DOMString markName, optional PerformanceMarkOptions markOptions = {})` — one
   REQUIRED argument, so a bare `new PerformanceMark()` is the TypeError Web IDL raises for it. */
static int js_mark_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == UT_CTOR_BUILD,
           "the PerformanceMark constructor resumed at a stage §2.2.1 does not have — it has one, and nothing "
           "in its eight steps reaches the page's code to rest at");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor PerformanceMark requires 'new'"), -1;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PerformanceMark requires a name"), -1;
    obj = mark_construct(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(obj)) return -1;
    *presult = obj;
    return 0;
}

/* USER TIMING §2.1.1's door: "Stores a timestamp with the associated name (a "mark"). It MUST run these
   steps:" — step 1 is the constructor above, step 2 is PERFORMANCE TIMELINE §5.1, step 4 is the return, and
   step 3 alone is user_timing.h's named residual. */
static int js_perf_mark_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == UT_MARK_BUILD, "§2.1.1's mark() resumed at a stage it does not have");
    DCHECK(performance_is(hdr->this_val),
           "§2.1.1's mark() ran on a receiver that is not a Performance — the declaration states Web IDL §3.7 "
           "Interfaces' implementation check, so reaching the body means idl_implementation_check did not run");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "mark requires a name"), -1;
    obj = mark_construct(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED);   /* STEP 1 */
    if (JS_IsException(obj)) return -1;
    /* STEP 2: "Queue a PerformanceEntry entry". It is HERE and not in §2.2.1's constructor above, which is the
       standard's own split: `new PerformanceMark('a')` mints an entry and puts it on no timeline, while
       `performance.mark('a')` queues it — so a PerformanceObserver observing `mark` sees the second and not
       the first. STEP 3, "Add entry to the performance entry buffer", is the residual user_timing.h names. */
    performance_observer_queue_entry(ctx, obj);
    *presult = obj;   /* STEP 4 */
    return 0;
}

static const IdlStepDecl js_mark_ctor_decl = {
    js_mark_ctor_step, sizeof(JSMarkCtorState), js_mark_visit, NULL,
    "USER TIMING §2.2.1 new PerformanceMark(markName, markOptions)", UT_CTOR_STEPS
};
static const IdlStepDecl js_perf_mark_decl = {
    js_perf_mark_step, sizeof(JSMarkCtorState), js_mark_visit, NULL,
    "USER TIMING §2.1.1 Performance.mark(markName, markOptions)", UT_MARK_STEPS
};

/* ---- §2.3 The PerformanceMeasure Interface -----------------------------------------------------------------
 *
 * ONE ADDED FIELD OVER §3's FOUR, exactly as §2.2's PerformanceMark has: `readonly attribute any detail`. The
 * two interfaces are the same shape and are NOT folded into one record, because the BRAND is the class
 * pointer and a shared record would make `PerformanceMark.prototype.detail` reachable on a measure. */
typedef struct {
    PerfEntry base;
    JSValue   detail;   /* §2.3's detail — OWNED */
} PerfMeasure;

static const uint16_t MEASURE_VALS[] = { (uint16_t)offsetof(PerfMeasure, base.name),
                                         (uint16_t)offsetof(PerfMeasure, base.entry_type),
                                         (uint16_t)offsetof(PerfMeasure, base.start_time),
                                         (uint16_t)offsetof(PerfMeasure, detail) };
static const CowRecord MEASURE_REC = { sizeof(PerfMeasure), MEASURE_VALS, 4 };

static void measure_release(JSRuntime *rt, void *derived)
{
    JS_FreeValueRT(rt, ((PerfMeasure *)derived)->detail);
}

static void measure_gc_mark(JSRuntime *rt, void *derived, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, ((PerfMeasure *)derived)->detail, mark_func);
}

static const PerfEntryClass MEASURE_CLASS = { "PerformanceMeasure", &MEASURE_REC, measure_release,
                                              measure_gc_mark };

static JSClassID g_measure_proto_slot;   /* a per-realm PROTOTYPE SLOT — see §2.2's, and performance_entry.h */
static int g_id_measure = -1;            /* §2.1.3's measure() machine */
static int g_id_clear_marks = -1;        /* §2.1.2's clearMarks() machine */
static int g_id_clear_measures = -1;     /* §2.1.4's clearMeasures() machine */

bool performance_measure_is(JSValueConst v)
{
    return performance_entry_is_a(v, &MEASURE_CLASS);
}

/* §2.3's `detail`, whose sentence is §2.2's with one word changed: "The detail attribute must return the value
   it is set to (it's copied from the PerformanceMeasureOptions dictionary)." The brand is stated here and is a
   real TypeError for the reason §2.2's getter gives — a receiver is PAGE-SUPPLIED INPUT and an assert on one
   hands a page an abort switch. */
static JSValue js_measure_get_detail(JSContext *ctx, JSValueConst this_val, int magic)
{
    PerfEntry *e;

    (void)magic;
    if (!performance_measure_is(this_val))
        return JS_ThrowTypeError(ctx, "PerformanceMeasure.detail was reached on something that is not a "
                                      "PerformanceMeasure");
    e = performance_entry_of(this_val);
    DCHECK(e != NULL, "a value that passed §2.3's brand answered no record — the brand IS a read of the record");
    return JS_DupValue(ctx, ((PerfMeasure *)e)->detail);
}

/* ---- §3 Processing: §3.2 Convert a name to a timestamp -----------------------------------------------------
 *
 * "To convert a name to a timestamp given a name that is a read only attribute in the PerformanceTiming
 * interface, run these steps:" — step 1 refuses a non-Window global, step 2 answers `navigationStart` with a
 * CONSTANT, and steps 3-5 read two attributes off NAVIGATION TIMING §8.1 The PerformanceTiming interface.
 *
 * STEP 2 IS WHY THIS COMPONENT NEEDS NO NAVIGATION TIMING AT ALL for the case pages actually write. Its own
 * words are "If name is navigationStart, return 0." — a constant of this standard, reached before any
 * milestone is read, so `performance.measure('m', 'navigationStart', 'x')` is answerable by a build that has
 * computed no navigation timing whatever.
 *
 * NAMED RESIDUAL — STEPS 3-5, EVERY OTHER PerformanceTiming NAME:
 *   WHAT IS NOT COVERED. §8.1's other twenty attributes. Steps 3 and 4 read `navigationStart` and `name` off
 *     the PerformanceTiming interface, which this build does not implement, so there is no value to subtract.
 *   WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. Step 5 is "If endTime is 0, throw an
 *     InvalidAccessError", and a milestone this user agent has never computed is one whose attribute a browser
 *     reports as 0 until it happens — so the throw below is the arm the standard itself takes for an
 *     unreached milestone rather than a stand-in for one. What is NARROWER is that this build takes it for
 *     milestones it HAS passed as well as for those it has not, because it records none of them.
 *   WHAT THE NEXT DIFF BUILDS. NAVIGATION TIMING §8.1's interface over a document load timing info, at which
 *     point steps 3 and 4 read real attributes and step 5 fires only for a milestone that has not happened.
 *   HOW ITS ABSENCE WOULD SHOW. A page measuring from a load milestone — `performance.measure('t',
 *     'responseEnd')` — gets an InvalidAccessError where a browser returns a duration, and sees it at every
 *     moment of the document's life rather than only before that milestone. */
static int ut_name_to_timestamp(JSContext *ctx, const char *name, double *out)
{
    JSValue global;
    bool is_window;

    global = JS_GetGlobalObject(ctx);
    is_window = window_is(global);
    JS_FreeValue(ctx, global);
    if (!is_window) {                                                     /* STEP 1 */
        JS_ThrowTypeError(ctx, "a PerformanceTiming attribute name may not be used as a mark outside a Window");
        return -1;
    }
    if (strcmp(name, "navigationStart") == 0) { *out = 0; return 0; }     /* STEP 2 */
    /* STEPS 3-5, over an interface this build does not carry — see the residual above. */
    JS_ThrowDOMException(ctx, "InvalidAccessError",
                         "this user agent has not computed the PerformanceTiming milestone this mark names, so "
                         "§3.2 step 5's endTime is 0");
    return -1;
}

/* ---- §2.1.3 measure(), and §3.1 Convert a mark to a timestamp inside it ------------------------------------
 *
 * §2.1.3's THREE CONVERSIONS ARE WHY THIS IS A STEP MACHINE AND NOT A BODY. §3.1's DOMString arm searches the
 * performance entry buffer for "the most recent occurrence of a PerformanceMark object ... whose name is
 * mark", and a mark NAME can be UNKNOWN EXTERNAL INPUT — `performance.mark(location.hash)` is the ordinary
 * spelling — so that search is a question over an unknown and MUST FORK rather than decide. A plain C body has
 * no machine state for the other arm to be snapshotted at, which solver/decide.h states outright: a fork from
 * one crashes at the seam naming the operation, and what that names is the declaration to build. §2.1.1's
 * mark() is already a step machine for the shape of its declaration; this one is a step machine because its
 * ALGORITHM parks.
 *
 * THE SEARCH IS AN ELIMINATION CHAIN AND IT IS core/idl_name_chain.c's, NOT A SECOND ONE. "Which member of a
 * set does this unknown name" is that component's whole subject, and the rule it holds is the one this search
 * would otherwise get wrong: each link is keyed by the member's OWN NAME and never by its RANK. A rank would
 * be a fact about the operand only where the set is fixed at its definition, and the performance entry buffer
 * is the opposite of that — every `performance.mark()` appends to it — so a replayed rank would name a
 * DIFFERENT mark at a second `measure()`, with every arm in range and every assert satisfied. */

/* THE SHARED NAME-EQUALITY PREDICATE, AND ITS SPELLING IS FROZEN.
   "Is this operand the name X" is ONE FACT, and §3.1, §2.1.2 and §2.1.4 all ask it of the same kind of
   operand — so they share one key, exactly as core/idl_index_arg.h's predicate is shared by eleven members
   because `index == 3` is one fact. Sharing is not a convenience: two keys over one operand would let a
   world answer YES under one and NO under the other, which is a world no input produces.
   THE STRING NAMES §3.1 BECAUSE THAT IS WHERE THE QUESTION WAS FIRST ASKED AND THE BYTES MAY NOT MOVE. A
   constraint key is what a parked flow's recorded answers are filed under, out of the IndexedDB cold tier
   and into the next session, so re-spelling it does not rename a question — it ORPHANS every answer already
   recorded against it. The macro's NAME is this file's and may change; the literal may not. */
#define UT_NAME_PREDICATE "USER TIMING §3.1 convert a mark to a timestamp (name ="
#define UT_MEASURE_ALGORITHM "USER TIMING §2.1.3 measure()"

typedef struct {
    JSStepHdr hdr;                    /* FIRST — the driver writes the def and the operand bounds through it */
    IdlNameChainSuppliedKey key;      /* the composed constraint key; the page supplies mark names, so it has
                                         no width and its storage is the growable one */
    JSValue  buffer;                  /* the `mark` buffer this walk is over — OWNED */
    JSValue  operand;                 /* the mark being converted — OWNED */
    JSValue  end_ts;                  /* §2.1.3 step 2's end time as a value — OWNED */
    JSValue  start_ts;                /* §2.1.3 step 3's start time as a value — OWNED */
    uint32_t cursor;                  /* how many buffer entries are still to be eliminated; the walk is
                                         most-recent-FIRST, so the entry under test is at cursor - 1 */
    uint8_t  started;                 /* HAVE THE OWNED FIELDS BEEN PLACED — its own byte and not a
                                         JS_IsUndefined test, because a step state arrives ZEROED and a zeroed
                                         JSValue is the INTEGER 0, so every value on a fresh state reads as set */
    uint8_t  phase;                   /* 0 = end time, 1 = start time, 2 = build */
    uint8_t  walking;                 /* the buffer walk for the conversion in flight has been set up */
} JSMeasureState;

static void js_measure_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSMeasureState *s = st;

    if (s->started) {
        v->val(ctx, &s->buffer);
        v->val(ctx, &s->operand);
        v->val(ctx, &s->end_ts);
        v->val(ctx, &s->start_ts);
    }
    /* THE KEY'S STORAGE IS NAMED THROUGH ITS OWN COMPONENT and never as a bare `buf` here — the pointer and
       the cap are one fact, and stating it twice is how they come apart. */
    idl_name_chain_supplied_visit(ctx, &s->key, v);
}

static void measure_state_start(JSContext *ctx, JSMeasureState *s)
{
    (void)ctx;
    if (s->started) return;
    s->buffer = JS_UNDEFINED;
    s->operand = JS_UNDEFINED;
    s->end_ts = JS_UNDEFINED;
    s->start_ts = JS_UNDEFINED;
    s->started = 1;
}

/* THE ENTRY UNDER TEST, and the base every one of them wears. */
static PerfEntry *ut_buffer_entry(JSContext *ctx, JSValueConst buffer, uint32_t i, JSValue *pheld)
{
    JSValue held = JS_GetPropertyUint32(ctx, (JSValue)buffer, i);
    PerfEntry *e = performance_entry_of(held);

    DCHECK(e != NULL,
           "§2's performance entry buffer holds something that is not a PerformanceEntry — §5.1 step 12 is its "
           "only writer and it appends the entry a producer had just minted");
    *pheld = held;
    return e;
}

/* §3.1 CONVERT A MARK TO A TIMESTAMP, over `s->operand`, into `*out` as a VALUE.
 *
 * IT ANSWERS A JSValue AND NOT A double, and that is the solver half rather than a convenience: a mark's
 * `startTime` is a JSValue on PERFORMANCE TIMELINE §3's record precisely because it can be unknown external
 * input, and handing back a C double here would launder exactly what that field exists to keep.
 *
 * RETURNS 0 with `*out` written (OWNED), -1 with a throw live, or a STEP CODE the caller must return
 * UNCHANGED — the flow is parked at a fork and the sibling's snapshot was taken there, so the caller's state
 * must be complete at the call.
 *
 * NAMED RESIDUAL — AN ENTRY IN THE BUFFER WHOSE OWN NAME IS UNKNOWN:
 *   WHAT IS NOT COVERED. A mark minted as `performance.mark(location.hash)` is in the buffer under a name this
 *     engine cannot spell, and a chain link is keyed by the member's own name. Such an entry is STEPPED OVER
 *     by the walk below rather than compared, so a `measure()` that should have resolved to it resolves to an
 *     older mark or throws §3.1's SyntaxError.
 *   WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. The two available alternatives are both wrong in the
 *     direction this engine exists to avoid: comparing the two unknowns with JS_IsStrictEqual DECIDES a
 *     predicate nothing observed, and spending `concolic_name_cstr` on the ENTRY's name to key a link would
 *     file two different marks' questions under one key whenever their shapes agree, so one link's recorded
 *     answer would decide another's. Stepping over it keeps every arm the run has.
 *   WHAT THE NEXT DIFF BUILDS. A link key composed from the ENTRY's own concolic identity
 *     (concolic_ident_c) rather than from its bytes, which is stable per source where a shape is not, plus
 *     the same over the searched operand so the two sides are named in one vocabulary.
 *   HOW ITS ABSENCE WOULD SHOW. A page that marks under an unknown name and then measures between two such
 *     marks raises SyntaxError where a browser resolves both, and the fork census records no link asked at
 *     §3.1 for a document whose marks are all injected. */
static int ut_mark_to_timestamp(JSContext *ctx, JSStepHdr *hdr, JSMeasureState *s, JSValue *out)
{
    if (!s->walking) {
        /* STEP 3's arm first, because it is the only one that is not about a DOMString: "Otherwise, if mark is
           a DOMHighResTimeStamp: If mark is negative, throw a TypeError. Otherwise, let end time be mark."
           A CONCOLIC IS NOT A NUMBER HERE even where its example is one — the value class is this engine's and
           the union arm that placed it has already been forked at the declaration — so it takes the DOMString
           arm, which is the arm §3.2.10's conversion CROSSES an unknown onto. */
        if (JS_IsNumber(s->operand) && !concolic_is(s->operand)) {
            double d = 0;

            CHECK(JS_ToFloat64(ctx, &d, s->operand) == 0, "§3.1 could not read a number it had just tested");
            if (d < 0) {
                JS_ThrowTypeError(ctx, "a mark given as a timestamp may not be negative");
                return -1;
            }
            *out = JS_DupValue(ctx, s->operand);
            return 0;
        }
        /* STEP 1: "If mark is a DOMString and it has the same name as a read only attribute in the
           PerformanceTiming interface, let end time be the value returned by running the convert a name to a
           timestamp algorithm with name set to the value of mark."
           THE TWENTY-ONE NAMES ARE THE ONES THIS FILE ALREADY HOLDS for §2.1.1 step 1 — one table, two
           readers, because they are the same fact about NAVIGATION TIMING §8.1 and a second copy is the one
           that drifts. An unknown denotes its SHAPE here for the same reason it does there: this is an
           EQUALITY against twenty-one fixed identifiers no shape spells, so the arm taken is the one the
           standard takes for every name that is not one of them. */
        {
            const char *n = concolic_name_cstr(ctx, s->operand);
            size_t i;
            bool timing_name = false;

            if (!n) return -1;
            for (i = 0; i < sizeof PERFORMANCE_TIMING_ATTRS / sizeof PERFORMANCE_TIMING_ATTRS[0]; i++)
                if (strcmp(n, PERFORMANCE_TIMING_ATTRS[i]) == 0) { timing_name = true; break; }
            if (timing_name) {
                double d = 0;
                int rc = ut_name_to_timestamp(ctx, n, &d);      /* §3.2 */

                JS_FreeCString(ctx, n);
                if (rc < 0) return -1;
                *out = JS_NewFloat64(ctx, d);
                return 0;
            }
            JS_FreeCString(ctx, n);
        }
        /* STEP 2's search. The buffer is THIS FLOW'S — every read of it goes through the COW delta — so it is
           held across the walk rather than re-read per link, and its length may not move while the walk is in
           flight: step_fork_run runs none of the page's code and the driver only clones and re-enters. */
        JS_FreeValue(ctx, s->buffer);   /* UNDEFINED by the caller's discipline; freed so it cannot leak if
                                           a later phase order ever changes that */
        s->buffer = performance_observer_buffer(ctx, "mark");
        s->cursor = 0;
        {
            JSValue lenv = JS_GetPropertyStr(ctx, s->buffer, "length");
            uint32_t n = 0;

            CHECK(JS_ToUint32(ctx, &n, lenv) == 0, "§2's performance entry buffer answered no length");
            JS_FreeValue(ctx, lenv);
            s->cursor = n;
        }
        s->walking = 1;
    }

    for (;;) {
        JSValue held, ex;
        PerfEntry *e;
        const char *member;
        int real, rc;
        bool yes = false, searched_unknown = concolic_is(s->operand) != 0;

        /* EVERY ENTRY ELIMINATED — §3.1 step 2's "If no matching entry is found, throw a SyntaxError". It is
           also the whole of the answer for an EMPTY buffer, which is why an empty one asks no question: one
           feasible completion is not a fork, it is the answer. */
        if (s->cursor == 0) {
            JS_ThrowDOMException(ctx, "SyntaxError",
                                 "§3.1 found no PerformanceMark in the performance entry buffer with this name");
            return -1;
        }
        e = ut_buffer_entry(ctx, s->buffer, s->cursor - 1, &held);
        /* THE RESIDUAL'S ARM — an entry this walk cannot name. */
        if (concolic_is(e->name)) {
            JS_FreeValue(ctx, held);
            s->cursor--;
            continue;
        }
        if (!searched_unknown) {
            /* BOTH SIDES ARE VALUES THE RUN DETERMINED, so the comparison is the interpreter's own and there
               is nothing to fork: §3.1's test is an equality and this world answers it. */
            bool same = JS_IsStrictEqual(ctx, s->operand, e->name);

            if (same) { *out = JS_DupValue(ctx, e->start_time); JS_FreeValue(ctx, held); return 0; }
            JS_FreeValue(ctx, held);
            s->cursor--;
            continue;
        }
        /* ONE LINK OF THE CHAIN. `member` is the ENTRY's own name, which the branch above has established is
           a value the run determined. `real` is which arm the operand's own EXAMPLE reaches, computed by
           RUNNING the comparison rather than by a rule predicting it, or the positive "I cannot say". */
        member = JS_ToCString(ctx, e->name);
        if (!member) { JS_FreeValue(ctx, held); return -1; }
        ex = concolic_example(ctx, s->operand);
        real = JS_IsUndefined(ex) ? JS_OUTCOME_REAL_UNSTATED : (JS_IsStrictEqual(ctx, ex, e->name) ? 1 : 0);
        JS_FreeValue(ctx, ex);
        rc = idl_name_chain_ask_supplied(ctx, hdr, &s->key, s->operand, UT_NAME_PREDICATE, member, real,
                                         UT_MEASURE_ALGORITHM, &yes);
        JS_FreeCString(ctx, member);
        if (rc) { JS_FreeValue(ctx, held); return rc; }
        if (yes) {
            *out = JS_DupValue(ctx, e->start_time);
            JS_FreeValue(ctx, held);
            return 0;
        }
        JS_FreeValue(ctx, held);
        s->cursor--;
    }
}

/* THE DICTIONARY ARM OF `(DOMString or PerformanceMeasureOptions)`. Web IDL §3.2.25 Union types decides it by
   asking whether V is an Object — a question a concolic answers `true` to for a reason that is about this
   engine's value class and not about the page's value — so core/idl_args.h FORKS that arm and both worlds
   run, and what arrives on the STRING world is the unknown ITSELF. This test is therefore the arm the
   declaration already resolved, read back, and not a second resolution of it. */
static bool ut_is_options(JSValueConst v)
{
    return JS_IsObject(v) && !concolic_is(v);
}

/* A RESOLVED TIMESTAMP AS THE double THE RECORD CAN HOLD, or false.
 *
 * NAMED RESIDUAL — A RESOLVED MARK WHOSE OWN startTime IS UNKNOWN:
 *   WHAT IS NOT COVERED. `performance.mark('a', {startTime: <unknown>})` puts a mark on the timeline whose
 *     §3 startTime is unknown external input, and §2.1.3 resolving to it has an end time it cannot subtract.
 *   WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. PERFORMANCE TIMELINE §3's record holds `end time` as a
 *     C double, and core/timing/performance_entry.c's duration getter asserts in its own words that the
 *     interface giving an entry a non-zero end time "resolves both ends to real timestamps before minting it".
 *     Minting anyway would fire that assert one read later, in a file that did not cause it; computing the
 *     duration from the example would publish a number the run never observed.
 *   WHAT THE NEXT DIFF BUILDS. `end time` as a JSValue on PerfEntry, beside `start_time`, so a duration can
 *     carry an unknown and §3's getter can subtract two values rather than two doubles.
 *   HOW ITS ABSENCE WOULD SHOW. A page whose marks take their startTime from injected state reaches a
 *     TypeError at `measure()` where a browser returns a PerformanceMeasure, and a dev build aborts naming
 *     this residual rather than the getter. */
static bool ut_timestamp_double(JSContext *ctx, JSValueConst v, double *out)
{
    if (!JS_IsNumber(v) || concolic_is(v)) return false;
    CHECK(JS_ToFloat64(ctx, out, v) == 0, "§3.1 answered a number that could not be read back");
    return true;
}

#define UT_MEASURE_STAGES(X) \
    X(UT_MEASURE_RESOLVE = IDL_STEP_FIRST, \
      "USER TIMING §2.1.3 measure() steps 1-12 (the three refusals, §3.1's end-time and start-time " \
      "conversions, and the PerformanceMeasure this returns) — the stage is re-entered once per link of " \
      "§3.1's elimination chain, which is the only thing in it that parks")
enum { UT_MEASURE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UT_MEASURE_STEPS[] = { UT_MEASURE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_perf_measure_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSMeasureState *s = st;
    JSValueConst name_arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst sou = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst end_mark = argc > 2 ? argv[2] : JS_UNDEFINED;
    bool opts = ut_is_options(sou);
    JSValue m_start, m_end, m_duration, m_detail;
    bool has_start, has_end, has_duration, has_detail, end_given;
    int rc;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == UT_MEASURE_RESOLVE, "§2.1.3's measure() resumed at a stage it does not have");
    DCHECK(performance_is(hdr->this_val),
           "§2.1.3's measure() ran on a receiver that is not a Performance — the declaration states Web IDL "
           "§3.7 Interfaces' implementation check, so reaching the body means idl_implementation_check did not "
           "run");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "measure requires a name"), -1;
    measure_state_start(ctx, s);

    /* THE DICTIONARY'S FOUR MEMBERS, READ ONCE PER ENTRY TO THIS STAGE. `idl_dict_get` answers `undefined` for
       a member with no entry, which is what `optional ... = {}` means and what every "member exists" test
       below reads. A resumed entry re-reads them rather than holding them on the state: they are the
       DECLARATION's converted values, the same objects, and re-reading runs none of the page's code. */
    m_detail   = opts ? idl_dict_get(ctx, sou, "detail")   : JS_UNDEFINED;
    m_duration = opts ? idl_dict_get(ctx, sou, "duration") : JS_UNDEFINED;
    m_end      = opts ? idl_dict_get(ctx, sou, "end")      : JS_UNDEFINED;
    m_start    = opts ? idl_dict_get(ctx, sou, "start")    : JS_UNDEFINED;
    has_detail = !JS_IsUndefined(m_detail);
    has_duration = !JS_IsUndefined(m_duration);
    has_end = !JS_IsUndefined(m_end);
    has_start = !JS_IsUndefined(m_start);
    end_given = !JS_IsUndefined(end_mark);

#define UT_MEASURE_FREE_MEMBERS() do { \
        JS_FreeValue(ctx, m_detail); JS_FreeValue(ctx, m_duration); \
        JS_FreeValue(ctx, m_end); JS_FreeValue(ctx, m_start); \
    } while (0)
#define UT_MEASURE_THROW(msg) do { \
        UT_MEASURE_FREE_MEMBERS(); \
        JS_ThrowTypeError(ctx, msg); \
        return -1; \
    } while (0)

    /* STEP 1's THREE CHECKS, each of them a TypeError and each of them the page's to see. They run on every
       entry to this stage rather than once, which costs nothing and is what keeps them true of the state a
       resumed flow is in. */
    if (opts && (has_start || has_end || has_duration || has_detail)) {
        if (end_given)
            UT_MEASURE_THROW("measure() may not be given an endMark beside a PerformanceMeasureOptions");
        if (!has_start && !has_end)
            UT_MEASURE_THROW("a PerformanceMeasureOptions with no start and no end member cannot be measured");
        if (has_start && has_duration && has_end)
            UT_MEASURE_THROW("a PerformanceMeasureOptions may not carry start, duration and end together");
    }

    /* STEPS 2 AND 3 AS A PHASE MACHINE, because each of them may need TWO §3.1 conversions and every one of
       those can park at a link of the chain. The phase is on the STATE, so a resumed entry re-enters at the
       conversion it was inside rather than re-running the ones it has already answered — and `walking` says
       whether the buffer walk for THAT conversion was already set up, which is what stops a resume restarting
       the walk at the top and re-asking links this flow has eliminated. */
    for (;;) {
        JSValueConst op = JS_UNDEFINED;
        JSValue resolved;
        double a = 0, b = 0;

        if (s->phase == 4) break;                                          /* on to steps 4-12 */
        switch (s->phase) {
        case 0:                                                            /* STEP 2: end time */
            /* "If endMark is given, let end time be the value returned by running the convert a mark to a
               timestamp algorithm passing in endMark." */
            if (end_given)                       { op = end_mark; break; }
            /* "Otherwise, if startOrMeasureOptions is a PerformanceMeasureOptions object, and if its end
               member exists, let end time be ... passing in startOrMeasureOptions's end." */
            if (opts && has_end)                 { op = m_end; break; }
            /* "Otherwise, if ... its start and duration members both exist" — the first of that pair. */
            if (opts && has_start && has_duration) { op = m_start; break; }
            /* "Otherwise, let end time be the value that would be returned by the Performance object's now()
               method." Which is THIS realm's §7 now(), asked of the component that owns it. */
            JS_FreeValue(ctx, s->end_ts);
            s->end_ts = performance_now_value(ctx);
            if (JS_IsException(s->end_ts)) { UT_MEASURE_FREE_MEMBERS(); return -1; }
            s->phase = 2;
            continue;
        case 1: op = m_duration; break;                                    /* STEP 2's `duration` of the pair */
        case 2:                                                            /* STEP 3: start time */
            /* "If startOrMeasureOptions is a PerformanceMeasureOptions object, and if its start member
               exists, let start time be ... passing in startOrMeasureOptions's start." */
            if (opts && has_start)                 { op = m_start; break; }
            /* "Otherwise, if ... its duration and end members both exist" — the first of that pair. */
            if (opts && has_duration && has_end)   { op = m_duration; break; }
            /* "Otherwise, if startOrMeasureOptions is a DOMString, let start time be ... passing in
               startOrMeasureOptions." The union's STRING arm, which is every value that is not the dictionary
               — including an unknown, which §3.2.10's conversion crosses unconverted. */
            if (!opts && !JS_IsUndefined(sou))     { op = sou; break; }
            /* "Otherwise, let start time be 0." */
            JS_FreeValue(ctx, s->start_ts);
            s->start_ts = JS_NewFloat64(ctx, 0);
            s->phase = 4;
            continue;
        case 3: op = m_end; break;                                         /* STEP 3's `end` of the pair */
        default:
            DFAIL("§2.1.3's measure() reached a phase its own machine does not declare");
            UT_MEASURE_FREE_MEMBERS();
            return JS_ThrowTypeError(ctx, "measure() reached an internal phase it does not declare"), -1;
        }
        if (!s->walking) {
            JS_FreeValue(ctx, s->operand);
            s->operand = JS_DupValue(ctx, op);
        }
        rc = ut_mark_to_timestamp(ctx, hdr, s, &resolved);
        if (rc) { UT_MEASURE_FREE_MEMBERS(); return rc; }
        /* THE WALK IS DONE WITH: its buffer reference and its cursor belong to ONE conversion, and a later
           phase that reused them would walk from wherever this one stopped. */
        s->walking = 0;
        JS_FreeValue(ctx, s->buffer);
        s->buffer = JS_UNDEFINED;
        switch (s->phase) {
        case 0:
            if (end_given || (opts && has_end)) {
                JS_FreeValue(ctx, s->end_ts);
                s->end_ts = resolved;
                s->phase = 2;
            } else {
                /* THE PAIR'S FIRST OPERAND, parked in `start_ts` until phase 1 resolves its `duration` and
                   adds the two. It is overwritten by step 3's own resolution afterwards. */
                JS_FreeValue(ctx, s->start_ts);
                s->start_ts = resolved;
                s->phase = 1;
            }
            break;
        case 1:
            /* "Let end time be start plus duration." Both ends are arithmetic on real timestamps; see
               ut_timestamp_double's residual for the one state that is not. */
            if (!ut_timestamp_double(ctx, s->start_ts, &a) || !ut_timestamp_double(ctx, resolved, &b)) {
                JS_FreeValue(ctx, resolved);
                UT_MEASURE_FREE_MEMBERS();
                DFAIL("§2.1.3 step 2 resolved a start or a duration that is not a real timestamp — see "
                      "ut_timestamp_double's residual: PERFORMANCE TIMELINE §3's record holds `end time` as a "
                      "C double, so the entry this would mint cannot carry the value");
                return JS_ThrowTypeError(ctx, "this engine cannot measure between marks whose timestamps are "
                                              "not real numbers"), -1;
            }
            JS_FreeValue(ctx, resolved);
            JS_FreeValue(ctx, s->end_ts);
            s->end_ts = JS_NewFloat64(ctx, a + b);
            s->phase = 2;
            break;
        case 2:
            /* EVERY ARM OF STEP 3 PARKS ITS RESULT IN THE SAME SLOT, and only the NEXT PHASE differs: the
               `duration`+`end` pair has a second operand to resolve, and the other two arms are done. */
            JS_FreeValue(ctx, s->start_ts);
            s->start_ts = resolved;
            s->phase = (opts && !has_start && has_duration && has_end) ? 3 : 4;
            break;
        case 3:
            /* "Let start time be end minus duration." `start_ts` is holding the DURATION this pair resolved
               first, which is why it is the subtrahend here and not the minuend. */
            if (!ut_timestamp_double(ctx, s->start_ts, &a) || !ut_timestamp_double(ctx, resolved, &b)) {
                JS_FreeValue(ctx, resolved);
                UT_MEASURE_FREE_MEMBERS();
                DFAIL("§2.1.3 step 3 resolved a duration or an end that is not a real timestamp — see "
                      "ut_timestamp_double's residual");
                return JS_ThrowTypeError(ctx, "this engine cannot measure between marks whose timestamps are "
                                              "not real numbers"), -1;
            }
            JS_FreeValue(ctx, resolved);
            JS_FreeValue(ctx, s->start_ts);
            s->start_ts = JS_NewFloat64(ctx, b - a);
            s->phase = 4;
            break;
        default:
            DFAIL("§2.1.3's measure() completed a conversion in a phase that asks for none");
            JS_FreeValue(ctx, resolved);
            UT_MEASURE_FREE_MEMBERS();
            return JS_ThrowTypeError(ctx, "measure() completed a conversion it did not ask for"), -1;
        }
    }

    /* ---- STEPS 4-12 ---------------------------------------------------------------------------------------
     *
     * NAMED RESIDUAL — A NEGATIVE DURATION WHOSE END TIME IS EXACTLY 0:
     *   WHAT IS NOT COVERED. Step 8's "The resulting duration value MAY be negative" holds for every pair
     *     except one: PERFORMANCE TIMELINE §3's record carries `end time` as a double whose value 0 is also
     *     its "no end time" sentinel, and core/timing/performance_entry.c's duration getter answers 0 for it
     *     without subtracting. So `performance.measure('m', 'a', 'navigationStart')` — whose end time §3.2
     *     step 2 answers with the constant 0 — reads duration 0 where the subtraction gives minus the mark's
     *     startTime.
     *   WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. That arm is what makes §2.2.1 step 6's "Set
     *     entry's duration attribute to 0" true of every PerformanceMark, so it cannot be deleted here; and
     *     computing the subtraction at this site instead would be a second answer to §3's own getter.
     *   WHAT THE NEXT DIFF BUILDS. A `has_end_time` bit on PerfEntry beside `end_time`, written by the mint
     *     and read by the getter in place of the `== 0` test, which makes the collision impossible rather
     *     than documented. It is a change to the BASE record and to every mint of it, which is why it is not
     *     folded into this one.
     *   HOW ITS ABSENCE WOULD SHOW. A measure whose end time is 0 and whose start time is not reads
     *     `duration === 0` where a browser reads a negative number, and `entry.startTime + entry.duration`
     *     does not equal its end. */
    {
        JSValue proto, obj, detail, entry_type;
        PerfMeasure *m;
        double end_d = 0;

        if (!ut_timestamp_double(ctx, s->end_ts, &end_d)) {
            UT_MEASURE_FREE_MEMBERS();
            DFAIL("§2.1.3 step 8 has an end time that is not a real timestamp — see ut_timestamp_double's "
                  "residual");
            return JS_ThrowTypeError(ctx, "this engine cannot measure to a mark whose timestamp is not a real "
                                          "number"), -1;
        }
        /* STEP 9: "If startOrMeasureOptions is a PerformanceMeasureOptions object and startOrMeasureOptions's
           detail member exists: Let record be the result of calling the StructuredSerialize algorithm on
           startOrMeasureOptions's detail. Set entry's detail to the result of calling the
           StructuredDeserialize algorithm on record and the current realm. Otherwise, set it to null."
           WRITTEN AS THE TWO ARMS AND NOT AS A `||`, for §2.2.1's reason: a default filled at a READ is the
           shape that cannot be told from a measurement. The serialize REFUSES a function, a Proxy, a Promise
           or a platform object with a "DataCloneError" DOMException, and that throw is the page's to see. */
        if (!has_detail || JS_IsNull(m_detail)) {
            detail = JS_NULL;
        } else {
            detail = structured_clone(ctx, m_detail);
            if (JS_IsException(detail)) { UT_MEASURE_FREE_MEMBERS(); return -1; }
        }
        UT_MEASURE_FREE_MEMBERS();

        /* STEP 4: "Create a new PerformanceMeasure object (entry) with this's relevant realm." `ctx` IS that
           realm — a C member runs in the realm that DEFINED it, and the member was installed on this realm's
           Performance prototype. */
        proto = JS_GetClassProto(ctx, g_measure_proto_slot);
        DCHECK(!JS_IsNull(proto),
               "a PerformanceMeasure was minted in a realm that never ran its install — the only door onto "
               "this operation is a member that same install declared");
        m = calloc(1, sizeof *m);
        CHECK(m != NULL, "user timing: a PerformanceMeasure's record could not be allocated");
        /* STEP 6: "Set entry's entryType attribute to DOMString "measure"." A constant of this standard. */
        entry_type = JS_NewString(ctx, "measure");
        if (JS_IsException(entry_type)) {
            free(m);
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, detail);
            return -1;
        }
        m->detail = detail;
        /* STEPS 5, 7 and 8 are PERFORMANCE TIMELINE §3's "initialize a PerformanceEntry" over this record —
           step 5 is its name, step 7 its startTime, and step 8 its end time, from which §3's own duration
           getter subtracts. The start time is handed over as the VALUE it was resolved to rather than as a
           double, which is what keeps a startTime a page can still read as its own. */
        obj = performance_entry_new(ctx, proto, &MEASURE_CLASS, m, JS_DupValue(ctx, s->start_ts), entry_type,
                                    JS_DupValue(ctx, name_arg), end_d);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(obj)) { measure_release(JS_GetRuntime(ctx), m); free(m); return -1; }
        /* STEP 10: "Queue a PerformanceEntry entry" — PERFORMANCE TIMELINE §5.1. STEP 11, "Add entry to the
           performance entry buffer", is INSIDE that: §5.1 steps 9-12 are the buffer, so a second append here
           would put the measure on the timeline twice. The same reading core/timing/user_timing.h states for
           §2.1.1's step 3. */
        performance_observer_queue_entry(ctx, obj);
        *presult = obj;                                                    /* STEP 12 */
        return 0;
    }
#undef UT_MEASURE_THROW
#undef UT_MEASURE_FREE_MEMBERS
    /* UNREACHABLE — the block above returns on every path. Written as a should-never-happen rather than left
       to the compiler's flow analysis, so that a future edit which does fall through here cannot return -1
       with no exception live, which is the one shape a step machine's caller cannot tell from a real throw. */
    DFAIL("§2.1.3's measure() fell out of its own steps 4-12");
    return JS_ThrowTypeError(ctx, "measure() fell out of its own steps"), -1;
}

static const IdlStepDecl js_perf_measure_decl = {
    js_perf_measure_step, sizeof(JSMeasureState), js_measure_visit, NULL,
    "USER TIMING §2.1.3 Performance.measure(measureName, startOrMeasureOptions, endMark)", UT_MEASURE_STEPS
};


/* ---- §2.1.2 clearMarks() and §2.1.4 clearMeasures() -------------------------------------------------------
 *
 * ONE ALGORITHM WITH TWO DOORS, exactly as §2.2.1 and §2.1.1 are. The two sections differ in three nouns and
 * in nothing else: "If markName is omitted, remove all PerformanceMark objects from the performance entry
 * buffer. Otherwise, remove all PerformanceMark objects listed in the performance entry buffer whose name is
 * markName" against the same sentence with PerformanceMeasure and measureName. Two copies would be the dual
 * system this codebase forbids and the seam between them is where the drift would be, so the CLASS and the
 * entry type are this operation's parameters and the doors carry nothing else.
 *
 * IT REMOVES IN A SECOND PASS, AND THAT IS THE LOAD-BEARING PART RATHER THAN A TIDY-UP. An unknown name makes
 * "whose name is markName" a question this flow must FORK on, and a fork PARKS — so a walk that removed as it
 * went would mutate the buffer with a sibling's snapshot already taken at a cursor into it, and the sibling
 * would resume walking an array its parent had shortened underneath it. core/timing/timer.c's chain states
 * the same hazard for a map one entry shorter at a second call. So the chain runs over an UNMUTATED buffer
 * and the removal happens once the name is settled, when nothing can park any more.
 *
 * THE YES ARM PINS, WHICH IS WHAT KEEPS THE WORLDS FEASIBLE. §3.1 wants ONE entry and stops at its first YES;
 * these two want ALL of them, so a naive chain would ask "is it `a`?" and then "is it `b`?" of an operand a
 * YES has already determined, and a world answering YES to both is one no input produces. Once a link answers
 * YES at name X this flow KNOWS the operand is X, so every remaining entry is decided by comparing its name
 * with X — the interpreter's own comparison on bytes the run has determined, no further question asked. That
 * is §Solver's concretize-on-pin performed LOCALLY over the rest of this walk, and it is sound for the reason
 * that rule gives: the determination comes from a predicate THIS FLOW evaluated, about THIS value.
 *
 * NAMED RESIDUAL — AN ENTRY IN THE BUFFER WHOSE OWN NAME IS UNKNOWN:
 *   WHAT IS NOT COVERED. The same population §3.1's walk steps over, and for the same reason: a chain link is
 *     keyed by the member's own name, and an entry minted as `performance.mark(location.hash)` has none this
 *     engine may spell without filing two marks' questions under one key. Such an entry is never removed by
 *     the NAMED arm of either member.
 *   WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. The OMITTED arm removes it like any other, because it
 *     asks no question at all; only the named arm skips it, and skipping is the arm that keeps a mark the run
 *     cannot prove is the named one. Removing it instead would delete an entry on a comparison nothing made.
 *   WHAT THE NEXT DIFF BUILDS. The same one §3.1's residual names: a link key composed from the entry's own
 *     concolic identity rather than from its bytes.
 *   HOW ITS ABSENCE WOULD SHOW. A page that marks under an injected name and then clears that same name finds
 *     the mark still on the timeline — `performance.measure()` against it still resolves — where a browser
 *     has removed it. */

#define UT_CLEAR_MARKS_ALGORITHM "USER TIMING §2.1.2 clearMarks()"
#define UT_CLEAR_MEASURES_ALGORITHM "USER TIMING §2.1.4 clearMeasures()"

typedef struct {
    JSStepHdr hdr;                    /* FIRST — the driver writes the def and the operand bounds through it */
    IdlNameChainSuppliedKey key;
    JSValue  buffer;                  /* the tuple's buffer for this member's entry type — OWNED */
    JSValue  pinned;                  /* the name a YES arm determined, or UNDEFINED — OWNED */
    uint32_t cursor;                  /* entries still to be eliminated; the one under test is at cursor - 1 */
    uint8_t  started;                 /* HAVE THE OWNED FIELDS BEEN PLACED — see §2.1.3's machine for why this
                                         is its own byte and not a JS_IsUndefined test */
    uint8_t  walking;                 /* the elimination walk has been set up */
    uint8_t  settled;                 /* the walk is done: `pinned` holds the name, or nothing matched */
} JSClearState;

static void js_clear_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSClearState *s = st;

    if (s->started) {
        v->val(ctx, &s->buffer);
        v->val(ctx, &s->pinned);
    }
    idl_name_chain_supplied_visit(ctx, &s->key, v);
}

/* THE REMOVAL, over a buffer nothing can park inside any more. `keep` is NULL for the OMITTED arm — "remove
   all PerformanceMark objects" — and otherwise the name the arm above settled on.
   IT REWRITES THE ARRAY IN PLACE rather than replacing the tuple's slot, which keeps the buffer's IDENTITY:
   §4.2 step 7.5 and §5.1 step 12 both reach it through the tuple, and a caller holding the old Array across a
   swap would append into an array the map no longer names. Every write here is a property write, which is
   what the per-flow COW delta captures — see core/timing/performance_observer.c on why the buffer is a JS
   value at all. */
static void ut_clear_remove(JSContext *ctx, JSValueConst buffer, const PerfEntryClass *cls, JSValueConst keep)
{
    uint32_t n = 0, i, k = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, (JSValue)buffer, "length");

    CHECK(JS_ToUint32(ctx, &n, lenv) == 0, "§2's performance entry buffer answered no length");
    JS_FreeValue(ctx, lenv);
    for (i = 0; i < n; i++) {
        JSValue held = JS_GetPropertyUint32(ctx, (JSValue)buffer, i);
        PerfEntry *e = performance_entry_of(held);
        bool remove;

        DCHECK(e != NULL, "§2's performance entry buffer holds something that is not a PerformanceEntry");
        /* THE BUFFER FOR AN ENTRY TYPE HOLDS ONLY THAT TYPE'S ENTRIES — §5.1 step 9 keys the tuple by the
           entry's own entryType — so this test is an ASSERTION of that keying rather than a filter that does
           work. Both sections say "remove all PerformanceMark objects", and in a per-type map that is the
           whole of the mark tuple. */
        DCHECK(e->cls == cls,
               "a performance entry tuple holds an entry of another interface — §5.1 step 9 keys the tuple by "
               "the entry's own entryType, so the two populations cannot differ unless a producer minted an "
               "entry whose entryType is not its interface's");
        remove = JS_IsUndefined(keep) ? true : JS_IsStrictEqual(ctx, e->name, keep);
        if (!remove) {
            if (k != i) JS_SetPropertyUint32(ctx, (JSValue)buffer, k, JS_DupValue(ctx, held));
            k++;
        }
        JS_FreeValue(ctx, held);
    }
    /* TRUNCATE. The kept entries were compacted to the front above, so setting `length` drops exactly the
       removed ones and releases their references. */
    JS_SetPropertyStr(ctx, (JSValue)buffer, "length", JS_NewUint32(ctx, k));
}

/* THE SHARED BODY. `entry_type` names the tuple, `cls` is the interface both sections name, and `algorithm` is
   the ADDRESS a should-never-happen inside the chain reports — §2.1.2's or §2.1.4's own spec identity rather
   than this file and this line, which is what core/idl_name_chain.h asks of every caller. */
static int ut_clear_step(JSContext *ctx, JSStepHdr *hdr, JSClearState *s, int argc, JSValueConst *argv,
                         const char *entry_type, const PerfEntryClass *cls, const char *algorithm)
{
    JSValueConst name = argc > 0 ? argv[0] : JS_UNDEFINED;

    if (!s->started) {
        s->buffer = JS_UNDEFINED;
        s->pinned = JS_UNDEFINED;
        s->started = 1;
    }
    /* STEP 1: "If markName is omitted, remove all PerformanceMark objects from the performance entry buffer."
       An OMITTED argument and an explicit `undefined` are one value here, which is what `optional DOMString
       markName` with no default means: the declaration places nothing, so the member is absent either way and
       the standard's "omitted" is the absence this reads. */
    if (JS_IsUndefined(name)) {
        JSValue buf = performance_observer_buffer(ctx, entry_type);

        ut_clear_remove(ctx, buf, cls, JS_UNDEFINED);
        JS_FreeValue(ctx, buf);
        return 0;
    }
    /* STEP 2 over a name the run DETERMINED — the comparison is the interpreter's own and nothing forks. */
    if (!concolic_is(name)) {
        JSValue buf = performance_observer_buffer(ctx, entry_type);

        ut_clear_remove(ctx, buf, cls, name);
        JS_FreeValue(ctx, buf);
        return 0;
    }
    /* STEP 2 over an UNKNOWN. The elimination walk first, over an unmutated buffer; the removal afterwards. */
    if (!s->walking && !s->settled) {
        JSValue lenv;
        uint32_t n = 0;

        JS_FreeValue(ctx, s->buffer);
        s->buffer = performance_observer_buffer(ctx, entry_type);
        lenv = JS_GetPropertyStr(ctx, s->buffer, "length");
        CHECK(JS_ToUint32(ctx, &n, lenv) == 0, "§2's performance entry buffer answered no length");
        JS_FreeValue(ctx, lenv);
        s->cursor = n;
        s->walking = 1;
    }
    while (!s->settled) {
        JSValue held, ex;
        PerfEntry *e;
        const char *member;
        int real, rc;
        bool yes = false;

        /* EVERY ENTRY ELIMINATED: this flow's world is the one in which the name matches nothing in the
           buffer, and both sections' answer for it is to remove nothing. */
        if (s->cursor == 0) { s->settled = 1; break; }
        held = JS_GetPropertyUint32(ctx, s->buffer, s->cursor - 1);
        e = performance_entry_of(held);
        DCHECK(e != NULL, "§2's performance entry buffer holds something that is not a PerformanceEntry");
        if (concolic_is(e->name)) {   /* the residual's arm — an entry this walk cannot name */
            JS_FreeValue(ctx, held);
            s->cursor--;
            continue;
        }
        member = JS_ToCString(ctx, e->name);
        if (!member) { JS_FreeValue(ctx, held); return -1; }
        ex = concolic_example(ctx, name);
        real = JS_IsUndefined(ex) ? JS_OUTCOME_REAL_UNSTATED : (JS_IsStrictEqual(ctx, ex, e->name) ? 1 : 0);
        JS_FreeValue(ctx, ex);
        rc = idl_name_chain_ask_supplied(ctx, hdr, &s->key, name, UT_NAME_PREDICATE, member, real,
                                         algorithm, &yes);
        JS_FreeCString(ctx, member);
        if (rc) { JS_FreeValue(ctx, held); return rc; }
        if (yes) {
            /* THE LOCAL PIN — see the banner. This world has determined the operand, so the rest of the
               removal asks nothing. */
            JS_FreeValue(ctx, s->pinned);
            s->pinned = JS_DupValue(ctx, e->name);
            s->settled = 1;
        }
        JS_FreeValue(ctx, held);
        if (!s->settled) s->cursor--;
    }
    /* NOTHING CAN PARK BELOW THIS LINE, which is what makes the mutation safe. */
    if (!JS_IsUndefined(s->pinned))
        ut_clear_remove(ctx, s->buffer, cls, s->pinned);
    JS_FreeValue(ctx, s->buffer);
    s->buffer = JS_UNDEFINED;
    s->walking = 0;
    return 0;
}

#define UT_CLEAR_MARKS_STAGES(X) \
    X(UT_CLEAR_MARKS_RUN = IDL_STEP_FIRST, \
      "USER TIMING §2.1.2 clearMarks() steps 1-3 — re-entered once per link of the elimination chain an " \
      "unknown markName asks, which is the only thing in it that parks")
enum { UT_CLEAR_MARKS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UT_CLEAR_MARKS_STEPS[] = { UT_CLEAR_MARKS_STAGES(JS_STEP_STAGE_LABEL) NULL };

#define UT_CLEAR_MEASURES_STAGES(X) \
    X(UT_CLEAR_MEASURES_RUN = IDL_STEP_FIRST, \
      "USER TIMING §2.1.4 clearMeasures() steps 1-3 — re-entered once per link of the elimination chain an " \
      "unknown measureName asks, which is the only thing in it that parks")
enum { UT_CLEAR_MEASURES_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UT_CLEAR_MEASURES_STEPS[] = { UT_CLEAR_MEASURES_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE TWO DOORS. Each carries its section's three nouns and nothing else; the body above is the algorithm.
   STEP 3, "Return undefined", is the `*presult` these leave untouched — a step machine's result is undefined
   unless it writes one, which is what `undefined clearMarks(...)` declares. */
static int js_clear_marks_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                               JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)presult; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == UT_CLEAR_MARKS_RUN, "§2.1.2's clearMarks() resumed at a stage it does not have");
    DCHECK(performance_is(hdr->this_val),
           "§2.1.2's clearMarks() ran on a receiver that is not a Performance — the declaration states Web IDL "
           "§3.7 Interfaces' implementation check");
    return ut_clear_step(ctx, hdr, st, argc, argv, "mark", &MARK_CLASS, UT_CLEAR_MARKS_ALGORITHM);
}

static int js_clear_measures_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                  JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)presult; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == UT_CLEAR_MEASURES_RUN, "§2.1.4's clearMeasures() resumed at a stage it does not have");
    DCHECK(performance_is(hdr->this_val),
           "§2.1.4's clearMeasures() ran on a receiver that is not a Performance — the declaration states Web "
           "IDL §3.7 Interfaces' implementation check");
    return ut_clear_step(ctx, hdr, st, argc, argv, "measure", &MEASURE_CLASS, UT_CLEAR_MEASURES_ALGORITHM);
}

static const IdlStepDecl js_clear_marks_decl = {
    js_clear_marks_step, sizeof(JSClearState), js_clear_visit, NULL,
    "USER TIMING §2.1.2 Performance.clearMarks(markName)", UT_CLEAR_MARKS_STEPS
};
static const IdlStepDecl js_clear_measures_decl = {
    js_clear_measures_step, sizeof(JSClearState), js_clear_visit, NULL,
    "USER TIMING §2.1.4 Performance.clearMeasures(measureName)", UT_CLEAR_MEASURES_STEPS
};

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void user_timing_install(JSContext *ctx)
{
    JSValue base, proto, prev, ctor, perf_proto, global, mproto;

    prev = JS_GetClassProto(ctx, g_mark_proto_slot);
    DCHECK(JS_IsNull(prev), "user_timing_install ran twice in one realm — everything already holding the first "
                            "PerformanceMark.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* `interface PerformanceMark : PerformanceEntry` — a real prototype chain built over THIS realm's §3
       prototype, which is what Web IDL §3.7.3's proto-step assertion inside idl_interface_tag checks against
       browser/idl_inheritance.h. The chain is not decoration: §2.2 states its five §3 attributes by saying
       what §3's must return for a mark, so a page reads `name`, `entryType`, `startTime` and `duration` off
       the object below this one. */
    base = performance_entry_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PerformanceMark.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PerformanceMark");
    idl_install_accessor_no_user_code(ctx, proto, "detail", js_mark_get_detail, 0, -1);
    JS_SetClassProto(ctx, g_mark_proto_slot, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT — CONSTRUCTIBLE, because §2.2's IDL declares a constructor. A page that mints
       a mark with `new PerformanceMark(name)` gets exactly what `performance.mark(name)` returns, minus the
       two timeline steps §2.1.1 adds and this build does not have. */
    ctor = idl_step_constructor(ctx, "PerformanceMark", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the PerformanceMark interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "PerformanceMark", ctor);
    JS_FreeValue(ctx, global);

    /* §2.3's PerformanceMeasure — the same chain over THIS realm's §3 prototype, and NOT constructible: §2.3's
       IDL declares no constructor, so `new PerformanceMeasure()` is the TypeError Web IDL raises for an
       interface object without construct steps, while the NAME is still present for a page's `instanceof`. */
    base = performance_entry_proto(ctx);
    mproto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(mproto), "PerformanceMeasure.prototype could not be allocated");
    idl_interface_tag(ctx, mproto, "PerformanceMeasure");
    idl_install_accessor_no_user_code(ctx, mproto, "detail", js_measure_get_detail, 0, -1);
    JS_SetClassProto(ctx, g_measure_proto_slot, JS_DupValue(ctx, mproto));
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "PerformanceMeasure",
                                         idl_interface_object(ctx, "PerformanceMeasure", mproto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, mproto);

    /* §2.1.1's and §2.1.3's members, onto the PARTIAL's target — HIGH RESOLUTION TIME §7's prototype, for THIS
       realm. The component that owns §7 hands it over rather than this one reaching for a class id it does not
       own, and the ORDER that makes that safe is core/platform.c's row order, which core/realm.h states is the
       declaration order and therefore the dependency order. */
    perf_proto = performance_proto(ctx);
    idl_install_method(ctx, perf_proto, "mark", g_id_mark);
    idl_install_method(ctx, perf_proto, "measure", g_id_measure);
    idl_install_method(ctx, perf_proto, "clearMarks", g_id_clear_marks);
    idl_install_method(ctx, perf_proto, "clearMeasures", g_id_clear_measures);
    JS_FreeValue(ctx, perf_proto);
}

void user_timing_init(JSContext *ctx)
{
    JSClassDef d = { "PerformanceMark" };
    JSClassDef dm = { "PerformanceMeasure" };
    /* §2.1.1.1's PerformanceMarkOptions, in the order the IDL declares its members — which for this dictionary
       is also Web IDL §3.2.17's lexicographical read order, so the two cannot disagree. Neither member is
       required and neither has a default, so an absent one has NO ENTRY on the converted dictionary and
       idl_dict_get answers `undefined` for it: that absence is what §2.2.1 step 5.1's "member exists" test
       reads, and it is why `startTime` may not be given a declared default here. */
    static const IdlDictMember MARK_OPTIONS[] = {
        { "detail",    IDL_ANY,    false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "startTime", IDL_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    };
    static const IdlArgType MARK_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };

    DCHECK(g_mark_proto_slot == 0, "user_timing_init ran twice — §2.2's prototype slot and the two machines "
                                   "are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_mark_proto_slot);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_mark_proto_slot, &d) == 0,
          "PerformanceMark: the per-realm prototype slot could not be declared");
    /* NO FINALIZER AND NO gc_mark ON THIS CLASS, deliberately, and it is not an omission: no object ever WEARS
       it. An entry wears PERFORMANCE TIMELINE §3's class, which is where the record, its release and its mark
       live; this id exists only so quickjs's per-context prototype slot can hold §2.2's prototype per realm.
       The same split core/file/file_system_handle.c makes for its two derived interfaces. */

    /* §2.1.3.1's PerformanceMeasureOptions, in WEB IDL §3.2.17's LEXICOGRAPHICAL read order, which for THIS
       dictionary is NOT the order the IDL declares its members in — the IDL writes `detail`, `start`,
       `duration`, `end` and the conversion reads `detail`, `duration`, `end`, `start`. §2.1.1.1's two happen
       to agree and this one does not, which is why the order is stated here rather than copied from the IDL:
       the read order is what decides which of a page's getters runs first, so it is observable.
       `start` AND `end` ARE DECLARED IDL_ANY AND THAT IS A NAMED RESIDUAL, stated at the one place the type
       is written:
         WHAT IS NOT COVERED. Both are `(DOMString or DOMHighResTimeStamp)`, and core/idl_args.h carries no
           row for that union — IDL_STRING_OR_DICT is its dictionary-armed sibling and IDL_DOUBLE_OR_SEQUENCE
           its sequence-armed one. So the member arrives UNCONVERTED and §3.1 tests its type in the body.
         WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. Web IDL §3.2.25's resolution for that union is
           "if V is a Number, convert to double; otherwise convert to DOMString", which is exactly the test
           §3.1's own steps 2 and 3 make of the value — so on a value the run DETERMINED the two agree in
           everything a page can see. What they do NOT agree on is an UNKNOWN: a declared union forks its arm
           (idl_concolic_rule) and this position cannot, so an unknown takes the DOMString arm here where a
           declared one would run both worlds.
         WHAT THE NEXT DIFF BUILDS. An `IDL_DOMSTRING_OR_DOUBLE` row in core/idl_args.h, listed by
           idl_type_has_dict's siblings and answering IDL_CONCOLIC_FORKS, with these two members declared as
           it. That file is outside this component, which is why the row is named here and not written.
         HOW ITS ABSENCE WOULD SHOW. `performance.measure('m', {start: injected, end: 5})` explores one world
           where a browser's union resolution and this engine's fork would give two, and the fork census
           records no arm asked at this position for a document whose measure options come from injected
           state. */
    static const IdlDictMember MEASURE_OPTIONS[] = {
        { "detail",   IDL_ANY,    false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "duration", IDL_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "end",      IDL_ANY,    false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "start",    IDL_ANY,    false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    };
    /* `PerformanceMeasure measure(DOMString measureName, optional (DOMString or PerformanceMeasureOptions)
       startOrMeasureOptions = {}, optional DOMString endMark)`. */
    static const IdlArgType MEASURE_ARGS[3] = { IDL_DOMSTRING, IDL_STRING_OR_DICT, IDL_DOMSTRING };

    g_ctor_stepid = idl_method_id_step(ctx, MARK_ARGS, 2, MARK_OPTIONS, 2, &js_mark_ctor_decl, 0);
    idl_optional_from(1);
    g_id_mark = idl_method_id_step(ctx, MARK_ARGS, 2, MARK_OPTIONS, 2, &js_perf_mark_decl, 0);
    idl_optional_from(1);
    /* §2.1.1's member is on a PARTIAL of §7's interface, so its receiver is a Performance and Web IDL §3.7.7
       Operations asks that BEFORE argument conversion — which is why it is stated at the declaration and not
       in the body. The constructor above states none: `new` has no receiver to check. */
    idl_this_iface(performance_is, "Performance");

    g_id_measure = idl_method_id_step(ctx, MEASURE_ARGS, 3, MEASURE_OPTIONS,
                                      (int)(sizeof MEASURE_OPTIONS / sizeof MEASURE_OPTIONS[0]),
                                      &js_perf_measure_decl, 0);
    idl_optional_from(1);
    idl_this_iface(performance_is, "Performance");

    /* `undefined clearMarks(optional DOMString markName)` and `undefined clearMeasures(optional DOMString
       measureName)` — one optional argument each and no dictionary, so an omitted one is the absence §2.1.2
       step 1 and §2.1.4 step 1 read. */
    {
        static const IdlArgType CLEAR_ARGS[1] = { IDL_DOMSTRING };

        g_id_clear_marks = idl_method_id_step(ctx, CLEAR_ARGS, 1, NULL, 0, &js_clear_marks_decl, 0);
        idl_optional_from(0);
        idl_this_iface(performance_is, "Performance");
        g_id_clear_measures = idl_method_id_step(ctx, CLEAR_ARGS, 1, NULL, 0, &js_clear_measures_decl, 0);
        idl_optional_from(0);
        idl_this_iface(performance_is, "Performance");
    }

    JS_NewClassID(JS_GetRuntime(ctx), &g_measure_proto_slot);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_measure_proto_slot, &dm) == 0,
          "PerformanceMeasure: the per-realm prototype slot could not be declared");

    agent_state_class("user_timing", &g_mark_proto_slot,
                      "USER TIMING §2.2's PerformanceMark per-realm prototype slot, and this component's "
                      "declaration latch");
    agent_state_id("user_timing", &g_ctor_stepid, "§2.2.1's constructor machine");
    /* PERFORMANCE TIMELINE §4.5's SET, DECLARED BY THE PRODUCER — see core/timing/performance_observer.h for
       why the membership is stated here and not listed there. This build's mint for the type is §2.2.1's
       constructor, reached through both of this component's doors; the TIMING ENTRY TYPES REGISTRY's row for
       "mark" reads PerformanceMark, availableFromTimeline True, maxBufferSize Infinite and should add entry
       "Return true", which is what §5.1 step 7.1.1 answers with. The literal is a static, which is what lets
       §4.5's array keep the pointer rather than a copy. */
    performance_observer_declare_entry_type("mark", INFINITY);
    /* §2.3's type, declared beside §2.2's and by the same producer. The TIMING ENTRY TYPES REGISTRY's row for
       "measure" reads PerformanceMeasure, availableFromTimeline True, maxBufferSize Infinite and should add
       entry "Return true" — the same three columns as "mark", which is why §5.6 answers false for both and
       §5.1 step 12 always appends. */
    performance_observer_declare_entry_type("measure", INFINITY);
    agent_state_id("user_timing", &g_id_mark, "§2.1.1's mark() machine");
    agent_state_id("user_timing", &g_id_measure, "§2.1.3's measure() machine");
    agent_state_id("user_timing", &g_id_clear_marks, "§2.1.2's clearMarks() machine");
    agent_state_id("user_timing", &g_id_clear_measures, "§2.1.4's clearMeasures() machine");
    agent_state_class("user_timing", &g_measure_proto_slot, "§2.3's PerformanceMeasure prototype slot");
    realm_declare_intrinsic(user_timing_install);
}

void user_timing_free(void)
{
    /* The prototypes and the interface objects are the REALMS' and go with their contexts; a mark's record is
       released by §3's finalizer. What the agent holds is one class id and two declarations, in a runtime that
       is going away with them. The id goes back to 0 because it is also this file's init latch — see
       core/agent_state.h — and carrying it would make a second agent's user_timing_init return before
       re-registering the slot, leaving every realm of that agent without a PerformanceMark.prototype. */
    g_mark_proto_slot = 0;
    g_ctor_stepid = -1;
    g_id_mark = -1;
}
