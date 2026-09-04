/* USER TIMING §2 — §2.1.1 mark(), §2.2 The PerformanceMark Interface, §2.2.1 The PerformanceMark Constructor.
   See user_timing.h for what is built, what is a named residual, and why. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/frame/window.h"
#include "core/timing/performance.h"
#include "core/timing/performance_entry.h"
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
      "USER TIMING §2.1.1 mark() steps 1 and 4 (run the PerformanceMark constructor, and return the entry); " \
      "steps 2 and 3 queue it onto a timeline this build does not have — see user_timing.h")
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
   steps:" — step 1 is the constructor above, step 4 is the return, and steps 2 and 3 are user_timing.h's
   named residual. */
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

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void user_timing_install(JSContext *ctx)
{
    JSValue base, proto, prev, ctor, perf_proto, global;

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
    JS_SetPropertyStr(ctx, global, "PerformanceMark", ctor);
    JS_FreeValue(ctx, global);

    /* §2.1.1's member, onto the PARTIAL's target — HIGH RESOLUTION TIME §7's prototype, for THIS realm. The
       component that owns §7 hands it over rather than this one reaching for a class id it does not own, and
       the ORDER that makes that safe is core/platform.c's row order, which core/realm.h states is the
       declaration order and therefore the dependency order. */
    perf_proto = performance_proto(ctx);
    idl_install_method(ctx, perf_proto, "mark", g_id_mark);
    JS_FreeValue(ctx, perf_proto);
}

void user_timing_init(JSContext *ctx)
{
    JSClassDef d = { "PerformanceMark" };
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

    g_ctor_stepid = idl_method_id_step(ctx, MARK_ARGS, 2, MARK_OPTIONS, 2, &js_mark_ctor_decl, 0);
    idl_optional_from(1);
    g_id_mark = idl_method_id_step(ctx, MARK_ARGS, 2, MARK_OPTIONS, 2, &js_perf_mark_decl, 0);
    idl_optional_from(1);
    /* §2.1.1's member is on a PARTIAL of §7's interface, so its receiver is a Performance and Web IDL §3.7.7
       Operations asks that BEFORE argument conversion — which is why it is stated at the declaration and not
       in the body. The constructor above states none: `new` has no receiver to check. */
    idl_this_iface(performance_is, "Performance");

    agent_state_class("user_timing", &g_mark_proto_slot,
                      "USER TIMING §2.2's PerformanceMark per-realm prototype slot, and this component's "
                      "declaration latch");
    agent_state_id("user_timing", &g_ctor_stepid, "§2.2.1's constructor machine");
    agent_state_id("user_timing", &g_id_mark, "§2.1.1's mark() machine");
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
