/* HIGH RESOLUTION TIME Level 3 §7 The Performance interface and §8.1 The performance attribute. See
   performance.h for why this is a component beside core/timing/hr_time.c rather than inside it, what a page got
   while it did not exist, and what of `performance` is still honestly absent. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/timing/hr_time.h"
#include "core/timing/performance.h"
#include "solver/cow.h"        /* the instance's record is a component's own C state — it time-travels */
/* THIS COMPONENT NO LONGER NAMES core/workers/, AND THAT IS THE POINT RATHER THAN A TIDY-UP. It used to
   include that header for two things — the §3.7.3 prototype `performance` lands on in a worker realm and
   `WorkerGlobalScope`'s own brand for §3.7.6's `target` — so an HR-TIME component depended on the worker
   layer for a pair of facts neither of them owns. The worker layer REGISTERS both with core/idl_args.c now
   and the install below asks for its member by name, so the dependency runs the way the layering allows. */

static JSClassID g_perf_class;
/* §8.1's "the Performance object" of THIS realm's global — AND THE CRASH THAT NAMED THIS SLOT FOR DELETION
   WAS WRONG ABOUT IT, recorded here because that clause is what the next reader would otherwise build to. The
   abort this file used to raise ended "so the member reads its environment off THIS, and DELETE THE REALM SLOT
   BELOW". Its spec half was exact and its remedy half named one mechanism too many: the slot was answering TWO
   questions, §8.1's "one object per realm" (a REGISTRY, which is correct and has no other home — "a page that
   stores `performance` and compares it later is comparing the same object") and "is this receiver from the
   member's realm" (a REALM TEST, which §3.7.6 does not ask and which is the defect). Deleting the slot would
   have taken §8.1's singleton with it; splitting the questions is the fix, and the record above is the half
   that was missing. RETIREMENT: this record goes when no reader can reach the deletion clause. */
static int g_perf_slot = -1;
static int g_id_now = -1;      /* §7.1 now() */
static int g_id_tojson = -1;   /* §7.3 toJSON(), which is Web IDL §3.7.7.1.1's algorithm */

/* THE INSTANCE CARRIES ITS OWN ENVIRONMENT, which is what §7.1 and §7.2 ask for in their own words: both
 * answer "given THIS's relevant global object", never the member's. A C member runs in the realm that DEFINED
 * it (js_call_c_function sets `ctx = p->u.cfunc.realm`), so the argument a member is handed is the realm of the
 * FUNCTION OBJECT the page reached it through — and Web IDL §3.7.6's own steps make those two different
 * questions on purpose: create an attribute getter is created "given an attribute attribute, a namespace or
 * interface target, and a realm realm" — TWO SEPARATE PARAMETERS — and its only refusal is "If jsValue does
 * not implement target", which is the INTERFACE and never the realm beside it. A
 * `Performance` of another realm implements `Performance`, so §3.7.6 ADMITS the call and what it decides is
 * only WHICH environment answers. That is why the receiver may not be asserted about: a receiver is
 * page-supplied input, and this component used to hand any page an abort switch over the whole engine by
 * DCHECKing that the receiver was this realm's own — `Object.getOwnPropertyDescriptor(frame.Performance.
 * prototype, "timeOrigin").get.call(performance)` aborted the run and took every finding with it.
 *
 * TWO FIELDS FOR ONE FACT, AND NEITHER DOES THE OTHER'S JOB. `realm` is the ENGINE's handle on the environment:
 * HR-TIME §4's time origin is a per-realm value keyed by context (core/realm.h), and §4's coarsening grid is
 * HTML §7.2.2.6's cross-origin isolated capability, which core/frame/agent_cluster.c answers off the realm's
 * own Document — so both of §7's environment reads take a JSContext and neither can be snapshotted here, the
 * second being a live question about a document a navigation can replace. `global` is §7's OWN operand and is
 * also what makes the handle SAFE: a raw JSContext in a record whose object can outlive its realm is a
 * use-after-free waiting for the reclamation core/frame/navigable.h is building, and a counted reference to the
 * realm's global closes it — the global's members are C function objects and each holds a counted reference to
 * the realm that defined them (quickjs.h's JS_SetContextMarkHook block states that cycle), so a live global is
 * a live realm. IT IS A DECLARED EDGE AND NOT A `JS_DupContext`: a context reference hung off an opaque is
 * invisible to gc_decref and would make the realm PERMANENTLY uncollectable, which is the defect that same
 * block exists to record; a JSValue in the layout below is marked, freed and dup'd like any other and the
 * collector can still break the cycle. Within one realm that cycle is the realm's own and roots nothing; a page
 * holding another realm's `performance` roots that realm, which is correct — the object has to be able to
 * answer from an environment that is therefore still there.
 *
 * EVERY CROSS-REALM RECEIVER THAT CAN REACH THIS IS SAME-AGENT, so `realm` is always a context of this runtime.
 * SECURITY.md keys an instance on `(browsing context group, origin)`, and HTML §7.2.1.3.1's
 * CrossOriginProperties(Window) — the fixed list a cross-origin WindowProxy exposes, which core/frame/ spells
 * once — does not carry `Performance`, so a cross-ORIGIN document's interface object is not reachable as a JS
 * object at all and the getter cannot be pulled off it. What IS reachable is a same-origin document of this
 * agent, which is one heap. */
typedef struct {
    JSContext *realm;    /* the environment settings object §7's members answer from. NOT a counted reference */
    JSValue    global;   /* §7.1/§7.2's "this's relevant global object" — OWNED, and what holds `realm` up */
} Performance;

/* THE ONE STATEMENT OF WHAT THE RECORD OWNS — the same list the finalizer frees and the gc_mark walks, which is
   why all three are written here together. `realm` is not in it: it is a pointer and not a JSValue, so the
   capture copies its BYTES with the rest of the record and never dups or frees it, which is exactly right for a
   handle whose lifetime the value beside it guarantees. */
static const uint16_t PERF_VALS[] = { (uint16_t)offsetof(Performance, global) };
static const CowRecord PERF_REC = { sizeof(Performance), PERF_VALS, 1 };

bool performance_is(JSValueConst v)
{
    return g_perf_class != 0 && JS_GetClassID(v) == g_perf_class;
}

/* THE ACCESSOR EVERY MEMBER REACHES THE RECORD THROUGH, and the capture is IN it for solver/cow.h's reason: a
   record a flow has REACHED is one it may write, the delta dedups to one entry per (flow, object), and there is
   then no write site left to miss. There is no write TODAY — both fields are stated once at the mint below and
   nothing else names them — and that is a fact this arrangement does not have to re-check the day a member
   starts writing one. The cost is bounded by §8.1's own shape rather than argued: there is exactly ONE
   Performance per realm, so this can never add more than one delta entry per realm per flow, which is why it is
   affordable on §7.1's own hot path (`while (performance.now() - t0 < 8)` is a spin every bundle writes).
   NOT performance_is: a brand check is a QUESTION, asked of values that are not Performances at all, and a
   question must not capture. */
static Performance *performance_rec(JSValueConst v)
{
    Performance *p = g_perf_class ? JS_GetOpaque(v, g_perf_class) : NULL;

    if (p) cow_capture_host_record(v, p, &PERF_REC);
    return p;
}

/* JS_GetAnyOpaque and not JS_GetOpaque in BOTH of these, deliberately: the collector dispatched here THROUGH
   the class, and this component's release column sets `g_perf_class` back to 0 before the runtime is torn down
   — see performance_free. Reading the static would make a finalizer running after that column answer NULL for
   a record that is there. */
static void perf_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    Performance *p = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(p != NULL, "a Performance was finalized with no record — §8.1's object has exactly one mint and it "
                      "attaches the record with nothing in between that could collect");
    JS_FreeValueRT(rt, p->global);
    free(p);
}

static void perf_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    Performance *p = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(p != NULL, "a Performance was marked with no record — its global is a counted reference and an "
                      "unmarked child is read by gc_scan as rooted from outside the heap, which would make "
                      "this realm uncollectable for the life of the runtime");
    JS_MarkValue(rt, p->global, mark_func);
}

/* WEB IDL §3.7.6 Attributes' BRAND, for the one member that is an ATTRIBUTE. THE NUMBER AND THE TITLE HERE
   WERE BOTH WRONG AND WERE WRONG SEPARATELY: it read "§3.7.5 Platform objects implementing interfaces", and
   §3.7.5 is Constants while "Platform objects implementing interfaces" is §3.8 — a pairing in which each half
   alone looks checkable and neither names the section that holds the step.
   §7's two operations state the same check at their declaration, where Web IDL §3.7.7 Operations asks it — step
   2.1.2.3 of the try-list, before step 2.1.4 computes the effective overload set — so a body cannot be the place
   for theirs. An attribute's getter has nothing to convert, so its receiver check has no ordering hazard and the
   plain-C getter form (idl_args.h: IdlGetter) has nowhere else to put it. A real TypeError and not an assert:
   a feature detector that pulls the descriptor and applies the getter to a bare object reads the throw as "this
   is a real interface", and tells it apart from `undefined`. */
static bool perf_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_perf_class != 0, "a Performance member ran before performance_init declared the class — the member "
                              "is only reachable through a prototype the per-realm install builds");
    if (performance_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "a Performance member was reached on something that is not a Performance");
    return false;
}

/* "THIS's RELEVANT GLOBAL OBJECT", ANSWERED RATHER THAN ASSERTED ABOUT — the environment §7.1 and §7.2 both
   name, taken off the RECEIVER. Every assert here is about a value THIS component wrote at the mint, which is
   the only thing a DCHECK may stand on; the receiver itself is page-supplied and its brand is a TypeError one
   line above every caller. */
static JSContext *perf_environment(JSValueConst this_val)
{
    Performance *p = performance_rec(this_val);

    DCHECK(p != NULL, "a Performance reached a member with no record — the brand is the class and the mint "
                      "attaches the record before the object leaves it, so a branded object without one came "
                      "from a second mint that does not exist");
    DCHECK(p->realm != NULL, "a Performance names no environment — §8.1's mint is the one writer of this field "
                             "and it writes the realm it is installing into");
    return p->realm;
}

/* §7.2 timeOrigin attribute: "MUST return the number of milliseconds in the duration returned by get time origin
   timestamp for the relevant global object of this". That operation is HR-TIME §4's and lives with §4 — this
   member is the whole of §7.2 and holds no arithmetic, which is the point of the split. */
static JSValue js_perf_time_origin(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!perf_brand(ctx, this_val)) return JS_EXCEPTION;
    /* THE ENVIRONMENT IS THE RECEIVER'S AND NOT `ctx` — see the record above. §4's get time origin timestamp
       reads the time origin OF THAT ENVIRONMENT, so the argument is what makes this member §7.2 rather than a
       member that answers for whichever realm the page reached the getter through. */
    return hr_time_origin_timestamp(perf_environment(this_val));
}

/* §7.1 now() method: "MUST return the number of milliseconds in the current high resolution time given this's
 * relevant global object (a duration)". §4's `current high resolution time` is hr_time_current, so this member
 * is that call and nothing else.
 *
 * §7.1's TWO OTHER SENTENCES ARE INVARIANTS OF THE CLOCK, NOT WORK FOR THIS MEMBER, and they hold here for
 * reasons stated where the clock is: "time values returned ... on Performance objects with the same time origin
 * MUST use the same monotonic clock" (there is exactly one — core/timing/event_loop.h's virtual clock, which
 * hr_time.c names as the whole of `unsafe shared current time`), and "the difference between any two
 * chronologically recorded time values ... MUST never be negative" (event_loop_advance_to asserts the clock
 * never decreases, and §4's floor is monotone, so the duration cannot).
 *
 * WHAT A PAGE MEASURES WITH IT IS NOT ZERO, AND THIS BLOCK SAID IT WAS FOR LONGER THAN IT WAS TRUE. It read
 * "WHAT A PAGE MEASURES WITH IT IS ZERO INSIDE ONE TASK", named the spin every bundle writes
 * (`while (performance.now() - start < 8) …`, a rAF budgeter, a requestIdleCallback polyfill), and said each
 * became a flow that never leaves its loop because the virtual clock advances only when a task source becomes
 * due. The clock's SECOND MOVER — the opcodes the running flow retired — had already landed in
 * core/timing/event_loop.c, so the sentence was false about this tree on the day it was written here, and it
 * was worse than merely wrong: it named a diff to build, in a file that already contained it, and a lane was
 * dispatched on it. A member's comment is the most-read description of the mechanism under it, so it is the
 * worst place for a claim about the mechanism's absence to go stale.
 *
 * WHAT THE SPIN DOES NOW, which is the whole reason this member is worth having: the loop's own iterations
 * retire opcodes, the interpreter banks them at its yield poll — and a back-edge raises a request, so a spin
 * polls every iteration — and the deadline the loop is waiting for is reached by the loop DOING THE WORK.
 * `while (performance.now() - start < 8)` exits. It is still preemptible bytecode throughout, so siblings run,
 * nothing is capped, and the flow parks and resumes at any depth inside it; the clock is a pure function of
 * the flow's own path, so the moment it resumes into is the moment it suspended in.
 *
 * A WALL-CLOCK READ HERE WOULD STILL BE THE WRONG REPAIR TWICE OVER, and that has not changed with the mover
 * arriving — it would make this member disagree with every other timestamp in the engine, and it would make a
 * resume stop being byte-identical, which §Testing's solver differential reports as a scheduling bug. That is
 * also why the mover's unit is opcodes retired and not CPU consumed: a CPU clock buys attribution and NOT
 * reproducibility, so the same flow on two machines would answer two moments. See hr_time.c's declaration of
 * the epoch estimate for the one place the wall clock legitimately enters this component — which is §7.2's
 * timeOrigin above, and only that. */
static JSValue js_perf_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)argc; (void)argv; (void)magic;
    /* The receiver's brand is the DECLARATION's (idl_this_iface below), so by here `this` is a Performance. */
    DCHECK(performance_is(this_val),
           "§7.1's now() ran on a receiver that is not a Performance — the declaration states Web IDL §3.7's "
           "implementation check, so reaching the body means idl_implementation_check did not run for it");
    /* §4's current high resolution time is "given a global object current global", and §7.1 names THIS's. */
    return hr_time_current(perf_environment(this_val));
}

/* §7.3 toJSON() method: "When toJSON() is called, run [WEBIDL]'s default toJSON steps." Web IDL §3.7.7.1.1
 * Default toJSON operation: build an ordered map by walking the inheritance stack base-first, and on each
 * interface — "if a toJSON operation with a [Default] extended attribute is declared on I" — take each exposed
 * regular attribute in order whose value is a JSON type.
 *
 * WHICH IS ONE ATTRIBUTE HERE, AND THE COUNT IS A FACT ABOUT THIS BUILD RATHER THAN A SHORTCUT. The stack is
 * [EventTarget, Performance]. EventTarget declares no toJSON at all, so §3.7.7.1.1's own condition excludes its
 * members before their types are even asked — and it has no attributes either way. Performance declares
 * `[Default] object toJSON()` and, in HR-TIME §7's IDL, exactly one regular attribute: `timeOrigin`, a
 * DOMHighResTimeStamp, which is a `double` and so a JSON type. A browser answers this with `timing` and
 * `navigation` beside it because Navigation Timing's legacy partial declares those two attributes on Performance;
 * they are ABSENT in this build (performance.h says why, and why absent rather than shaped), so their absence
 * from this object is the same honest report the interface itself makes. The day one of them lands, it lands in
 * THIS list too — which is the whole reason the list is written out of the attributes rather than hand-held. */
static JSValue js_perf_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue out, time_origin;

    (void)argc; (void)argv; (void)magic;
    DCHECK(performance_is(this_val),
           "§7.3's toJSON() ran on a receiver that is not a Performance — the declaration states Web IDL §3.7's "
           "implementation check, so reaching the body means idl_implementation_check did not run for it");
    /* §3.7.7.1.1 step 4's OrdinaryObjectCreate(%Object.prototype%), AND THE ONE LINE HERE FOR WHICH `ctx` IS
       THE RIGHT REALM. %Object.prototype% is an intrinsic with no realm stated, so ECMAScript takes the CURRENT
       realm — which for a §3.7.7 operation is the realm its function object was created in, i.e. the one the
       page reached this method through. The VALUE below still comes from the receiver's environment, because
       step 5 runs "the getter steps of attr with object as this" and that getter is the member above. */
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a Performance's toJSON result could not be allocated");
    /* "Let value be the result of running the getter steps of attr with object as this" — the GETTER, not a
       second path to the same number, so the map cannot disagree with the attribute. */
    time_origin = js_perf_time_origin(ctx, this_val, 0);
    if (JS_IsException(time_origin)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
    JS_SetPropertyStr(ctx, out, "timeOrigin", time_origin);
    return out;
}

/* THIS REALM'S `Performance.prototype`, and HIGH RESOLUTION TIME §7.1's answer, for the PARTIALS in other
   standards — see performance.h for why both are handed over rather than reached for. The assert is not
   ceremony: a partial installed onto a NULL would define nothing and the member would simply not be there,
   which is the silent half of the absence this whole component exists to stop lying about. */
JSValue performance_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_perf_class);

    DCHECK(!JS_IsNull(proto), "Performance.prototype was asked for in a realm that never ran performance_"
                              "install — a partial interface's member would be installed on nothing and would "
                              "be absent from that document with nothing to say so. The order is core/"
                              "platform.c's row order, which core/realm.h states IS the declaration order");
    return proto;
}

JSValue performance_now_value(JSContext *ctx)
{
    return hr_time_current(ctx);
}

/* §8.1 The performance attribute — `[Replaceable] readonly attribute Performance performance` on the
   WindowOrWorkerGlobalScope mixin, which "allows access to performance related attributes and methods from the
   global object". One object per realm, so this hands back the realm's rather than minting one per read: a page
   that stores `performance` and compares it later is comparing the same object, as it does in a browser. */
/* NAMED RESIDUAL — THIS ONE GETTER STILL ANSWERS FROM ITS OWN REALM, AND IT IS THE HALF THE RECORD ABOVE
   CANNOT REACH. §8.1 states no getter steps, so Web IDL §3.7.6's default applies and the value is the
   attribute's value ON `this` — the receiver's environment, exactly as §7.1 and §7.2 name theirs. The record
   above put that within reach for a Performance because a Performance's environment is written at the one mint
   this file owns; the receiver HERE is a Window or a WorkerGlobalScope, and nothing on one of those names its
   realm, so a member that wanted to read it off `this` has nothing to read.
   NOT COVERED: a §8.1 read reached through ANOTHER realm's accessor. The property is `[Replaceable]`, so it is
   an ordinary accessor a page can pull a descriptor off, and the getter it gets back is the one minted in the
   realm whose global it was taken from; applied to a different global it hands back the MINTING realm's
   Performance rather than the receiver's.
   WHAT THE NEXT DIFF BUILDS: the Window carrying its own environment the way the Performance above now does —
   which is core/frame/window.c's record and not this file's, because the value has to be reachable off the
   RECEIVER and this component never sees a Window being built. This getter then reads it off `this_val` and
   `ctx` stops being the answer here as it has already stopped being the answer above.
   HOW ITS ABSENCE WOULD SHOW: two same-origin documents of one agent, whose `performance` objects a browser
   keeps distinct, answer as ONE object when both are asked through a single descriptor's getter — an identity
   a page can test directly, with no member of either object read. */
static JSValue js_win_performance(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_perf_slot);
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* ONE PROTOTYPE, ONE INTERFACE OBJECT AND ONE Performance PER REALM, built WITH the realm. §3.7 gives every
   realm its own interface prototype object, and here that decides ANSWERS and not just identities: §7.1 and
   §7.2 answer out of the realm the member was DEFINED in, so a shared prototype would hand every document the
   first realm's clock and time origin. */
static void performance_install(JSContext *ctx)
{
    JSValue proto, prev, global, obj;
    Performance *perf;

    prev = JS_GetClassProto(ctx, g_perf_class);
    DCHECK(JS_IsNull(prev), "performance_install ran twice in one realm — everything already holding the first "
                            "Performance.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* `interface Performance : EventTarget` — a real prototype chain, so `addEventListener` on the object is
       DOM §2.7's and not a second listener list. The interface declares no event handler IDL attributes of its
       own in HR-TIME §7; the one a browser carries (`onresourcetimingbufferfull`) is Resource Timing's partial.
       IT DOES NOT ARRIVE WITH THE PERFORMANCE TIMELINE, which this used to say: Resource Timing §3.4 gives the
       global a resource timing buffer of its OWN, and it is that buffer filling — not the performance entry
       buffer map — that fires the event. It arrives with the resource timing entries this build does not mint.
       See the header, which states that decision beside the other three it was wrongly folded in with. */
    proto = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, proto, "Performance");
    idl_install_accessor(ctx, proto, "timeOrigin", js_perf_time_origin, 0, -1);
    idl_install_method(ctx, proto, "now", g_id_now);
    idl_install_method(ctx, proto, "toJSON", g_id_tojson);
    JS_SetClassProto(ctx, g_perf_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. Performance declares no constructor, so
       `new Performance()` is a TypeError — and its PRESENCE is what a feature-detecting bundle reads. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "Performance", idl_interface_object(ctx, "Performance", proto));

    /* §8.1's ONE Performance OF THIS REALM, and the ONE MINT of the record above — which is what lets
       perf_environment assert rather than test. The record is COMPLETE before it is attached and nothing
       between the allocation and JS_SetOpaque can collect, which is why the finalizer asserts it is there.
       `global` is DUP'd from the one already in hand: a second JS_GetGlobalObject would be a second answer to a
       question with one, and this way the value the record holds is provably the value the two §3.8 property
       references below are placed on. */
    obj = JS_NewObjectProtoClass(ctx, proto, g_perf_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's Performance object could not be allocated");
    perf = calloc(1, sizeof *perf);
    CHECK(perf != NULL, "this realm's Performance record could not be allocated");
    perf->realm = ctx;
    perf->global = JS_DupValue(ctx, global);
    JS_SetOpaque(obj, perf);
    realm_value_set(ctx, g_perf_slot, obj);

    /* HR-TIME §8.1 "The performance attribute" — `[Replaceable] readonly attribute Performance performance`,
       declared by `WindowOrWorkerGlobalScope`. WHICH OBJECT IT LANDS ON IS WEB IDL §3.7.3's CONDITIONAL AND
       NOT A REALM TEST. §2.3 "Interface mixins" makes a mixin's members the INCLUDING interface's own — "all
       objects implementing an interface I ... must additionally include the members of interface mixin M" —
       so this is a `Window` member in a Window realm and a `WorkerGlobalScope` member in a worker one, and
       §3.7.3's "If interface is not declared with the [Global] extended attribute" arm then puts it on the
       global in the first (Window IS [Global]) and on `WorkerGlobalScope.prototype` in the second.
       THE TWO ARMS USED TO BE WRITTEN HERE, SELECTED BY ASKING core/workers/ FOR A PROTOTYPE AND TESTING
       WHETHER ONE CAME BACK, AND THAT SPELLING IS RETIRED RATHER THAN MERELY MOVED — the argument that put it
       here is the argument a reader repeats. It said, correctly, that the two arms are ROUTING and not a
       fallback, because deleting either leaves §3.7.3 still to be asked of every realm. What it got wrong is
       WHAT IT ASKED: `did the worker component build a prototype in this realm` is a HAND-PICKED LIST OF THE
       REALM KINDS THIS ENGINE HAPPENS TO BUILD, not §3.7.3's conditional. browser/idl_exposure.h carries
       IDL_GLOBALS rows for ServiceWorkerGlobalScope, SharedWorkerGlobalScope and four WorkletGlobalScope
       interfaces; not one of their bands declares `performance`, and not one of them has a
       `WorkerGlobalScope.prototype`, so in every one of those realms the else-arm installed onto the GLOBAL —
       the §3.8 violation whose only symptom is a descriptor read. The entry below asks the GENERATED BAND,
       which engine/idlgen.mjs derives from the harvested IDL, and crashes by name where neither arm can be
       served.
       THE SETTER IS WHAT NEEDED THE BRAND, NOT THE GETTER, and the brand is no longer this component's to
       state. §3.7.6 "Attributes" gives create an attribute getter and create an attribute setter the same
       opening steps, and a plain-C getter minted on anything but the realm's global is minted RAW — so the
       getter runs none of them on either object today, which is this engine's standing state for every
       prototype accessor. The [Replaceable] SETTER runs all three wherever it is installed, and its step
       1.1.2.3 asks whether the receiver implements `target`; that predicate belongs to the interface that
       DECLARES the member, so it now travels with the object from the one registration rather than being
       named here, where this component had to reach into another directory to find it. */
    idl_install_replaceable_member(ctx, global, "performance", js_win_performance, 0);
    JS_FreeValue(ctx, global);
}

void performance_init(JSContext *ctx)
{
    JSClassDef d = { "Performance", perf_finalizer, perf_gc_mark };

    DCHECK(g_perf_slot < 0, "performance_init ran twice — the class, the slot and the two operations are "
                            "declared once per AGENT");
    /* THE CLASS IS THE PER-REALM PROTOTYPE SLOT, THE BRAND AND THE RECORD'S DOOR: the one object per realm
       WEARS it, so §3.7.6 Attributes' and §3.7.7 Operations' check is a class-id comparison a page cannot
       forge, and the same id is what reaches the environment that object carries. The brand answers WHICH
       INTERFACE, which is §3.7.6's `target`, and it has never been able to answer WHICH REALM — that is the
       record's, and conflating the two is what made a legal cross-realm call abort. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_perf_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_perf_class, &d) == 0,
          "Performance: the per-realm prototype slot could not be declared");
    g_perf_slot = realm_value_declare(ctx, "HR-TIME §8.1 the global object's Performance, of the interface §7 "
                                           "declares");

    /* Both operations take no arguments, so neither declaration has a type list; what each states is Web IDL
       §3.7 Interfaces' implementation-check an object, step 3, which §3.7.7 Operations asks BEFORE argument
       conversion — one line per member and never a bracket, for the reason core/indexeddb/idb_index_handle.c
       records. Plain C bodies and not step machines: each is one call into core/timing/hr_time.c over numbers
       and reaches none of the page's code, so there is nothing in either to suspend at. */
    g_id_now = idl_method_id(ctx, NULL, 0, js_perf_now, 0);
    idl_this_iface(performance_is, "Performance");
    g_id_tojson = idl_method_id(ctx, NULL, 0, js_perf_tojson, 0);
    idl_this_iface(performance_is, "Performance");

    agent_state_class("performance", &g_perf_class,
                      "HR-TIME §7 Performance's class — its per-realm prototype slot and Web IDL §3.7.6 "
                      "Attributes' and §3.7.7 Operations' brand");
    agent_state_realm_slot("performance", &g_perf_slot,
                           "HR-TIME §8.1's realm-value slot for the global's Performance, and this component's "
                           "declaration latch");
    agent_state_id("performance", &g_id_now, "HR-TIME §7.1 now()'s declaration");
    agent_state_id("performance", &g_id_tojson, "HR-TIME §7.3 toJSON()'s declaration");
    realm_declare_intrinsic(performance_install);
}

void performance_free(void)
{
    /* The prototypes, the interface objects and the Performance objects are the REALMS' — each is released with
       its context. What the agent holds is the class id, the realm slot and the two declarations, and all four
       are registrations in a runtime that is going away with them. The class id goes back to 0 because
       core/agent_state.h makes that one policy for every component: it doubles as this file's own init latch,
       so carrying it would make a second agent's performance_init RETURN before re-registering the class, and
       every Performance it minted would be branded with an id the live runtime never gave out.
       THE REASON THAT USED TO MAKE THAT SAFE IS RETIRED AND THE ANSWER IS THE SAME — rewritten rather than
       deleted, because a reader re-deriving it will re-derive the retired half. It read "safe here for that
       header's reason — the class HAS NO FINALIZER AND NO gc_mark, so nothing dispatched through it runs after
       this column", and this class has both now. What holds instead is core/timing/performance_entry.c's
       reason, which does not depend on the class having no hooks: a Performance's record is released by the
       finalizer, which runs AFTER this column, and both hooks reach it through JS_GetAnyOpaque and read no
       static of this file — so a zeroed class id is invisible to them. */
    g_perf_class = 0;
    g_perf_slot = -1;
    g_id_now = -1;
    g_id_tojson = -1;
}
