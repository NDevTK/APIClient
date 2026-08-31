/* HIGH RESOLUTION TIME Level 3 §7 The Performance interface and §8.1 The performance attribute. See
   performance.h for why this is a component beside core/timing/hr_time.c rather than inside it, what a page got
   while it did not exist, and what of `performance` is still honestly absent. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/timing/hr_time.h"
#include "core/timing/performance.h"

static JSClassID g_perf_class;
static int g_perf_slot = -1;   /* §8.1's "the Performance object" of THIS realm's global */
static int g_id_now = -1;      /* §7.1 now() */
static int g_id_tojson = -1;   /* §7.3 toJSON(), which is Web IDL §3.7.7.1.1's algorithm */

bool performance_is(JSValueConst v)
{
    return g_perf_class != 0 && JS_GetClassID(v) == g_perf_class;
}

/* WEB IDL §3.7.5 Platform objects implementing interfaces' BRAND, for the one member that is an ATTRIBUTE.
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

/* THE HALF OF "THIS's RELEVANT GLOBAL OBJECT" THIS ENGINE CAN ANSWER, asserted rather than assumed — see
   performance.h. Both §7.1 and §7.2 name it, and both would answer out of the MEMBER's realm rather than the
   RECEIVER's if a page ever crossed them. */
static void perf_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_perf_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "a Performance member was reached through ONE realm's Performance.prototype on ANOTHER realm's "
                 "Performance — §7.1 and §7.2 both answer for `this`'s relevant global object, so the clock and "
                 "the time origin would come out of the member's own document instead. BUILD the Performance "
                 "that carries its own realm: give the instance a record as its class opaque (with the "
                 "finalizer, gc_mark and cow_capture_host_record contract that entails) so the member reads its "
                 "environment off THIS, and delete the realm slot below");
}

/* §7.2 timeOrigin attribute: "MUST return the number of milliseconds in the duration returned by get time origin
   timestamp for the relevant global object of this". That operation is HR-TIME §4's and lives with §4 — this
   member is the whole of §7.2 and holds no arithmetic, which is the point of the split. */
static JSValue js_perf_time_origin(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!perf_brand(ctx, this_val)) return JS_EXCEPTION;
    perf_assert_this_realm(ctx, this_val);
    return hr_time_origin_timestamp(ctx);
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
 * WHAT A PAGE MEASURES WITH IT IS ZERO INSIDE ONE TASK, and this member makes that residual REACHABLE where it
 * was nearly theoretical. hr_time.c names it at `unsafe shared current time` and gives one way to hit it — a
 * `do { … } while (e2.timeStamp - e1.timeStamp === 0)` loop, which is one WPT file. `performance.now()` is the
 * spelling every bundle actually uses: a frame budgeter (`while (performance.now() - start < 8) …`), a
 * requestIdleCallback polyfill, a spin that waits for a deadline. Each of those becomes a flow that never leaves
 * its loop, because the virtual clock advances only when a task source becomes due and no task can become due
 * while one is running. THAT IS THE FORCING FUNCTION AND NOT A REASON TO SOFTEN THIS MEMBER: the flow is
 * preemptible bytecode, so nothing is capped and every sibling still runs; what the loop reports is that the
 * clock does not yet advance with WORK PERFORMED, which is the diff hr_time.c names at the site that owns it
 * (core/timing/event_loop.c, driven from the per-opcode attention check that already counts a flow's progress).
 * A wall-clock read HERE is the wrong repair twice over — it would make this member disagree with every other
 * timestamp in the engine, and it would make a resume stop being byte-identical, which §Testing's solver
 * differential reports as a scheduling bug. See hr_time.c's declaration of the epoch estimate for the one place
 * the wall clock legitimately enters this component and why it is only that one. */
static JSValue js_perf_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    /* The receiver's brand is the DECLARATION's (idl_this_iface below), so by here `this` is a Performance. */
    DCHECK(performance_is(this_val),
           "§7.1's now() ran on a receiver that is not a Performance — the declaration states Web IDL §3.7's "
           "implementation check, so reaching the body means idl_implementation_check did not run for it");
    perf_assert_this_realm(ctx, this_val);
    return hr_time_current(ctx);
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
    perf_assert_this_realm(ctx, this_val);
    /* §3.7.7.1.1 step 4's OrdinaryObjectCreate(%Object.prototype%) — a plain object of THIS realm. */
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a Performance's toJSON result could not be allocated");
    /* "Let value be the result of running the getter steps of attr with object as this" — the GETTER, not a
       second path to the same number, so the map cannot disagree with the attribute. */
    time_origin = js_perf_time_origin(ctx, this_val, 0);
    if (JS_IsException(time_origin)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
    JS_SetPropertyStr(ctx, out, "timeOrigin", time_origin);
    return out;
}

/* §8.1 The performance attribute — `[Replaceable] readonly attribute Performance performance` on the
   WindowOrWorkerGlobalScope mixin, which "allows access to performance related attributes and methods from the
   global object". One object per realm, so this hands back the realm's rather than minting one per read: a page
   that stores `performance` and compares it later is comparing the same object, as it does in a browser. */
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

    prev = JS_GetClassProto(ctx, g_perf_class);
    DCHECK(JS_IsNull(prev), "performance_install ran twice in one realm — everything already holding the first "
                            "Performance.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* `interface Performance : EventTarget` — a real prototype chain, so `addEventListener` on the object is
       DOM §2.7's and not a second listener list. The interface declares no event handler IDL attributes of its
       own in HR-TIME §7; the ones a browser carries (`onresourcetimingbufferfull`) are Resource Timing's
       partial, and they arrive with the timeline that fires them. */
    proto = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, proto, "Performance");
    idl_install_accessor(ctx, proto, "timeOrigin", js_perf_time_origin, 0, -1);
    idl_install_method(ctx, proto, "now", g_id_now);
    idl_install_method(ctx, proto, "toJSON", g_id_tojson);
    JS_SetClassProto(ctx, g_perf_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. Performance declares no constructor, so
       `new Performance()` is a TypeError — and its PRESENCE is what a feature-detecting bundle reads. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Performance", idl_interface_object(ctx, "Performance", proto));

    obj = JS_NewObjectProtoClass(ctx, proto, g_perf_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's Performance object could not be allocated");
    realm_value_set(ctx, g_perf_slot, obj);

    idl_install_replaceable(ctx, global, "performance", js_win_performance, 0);
    JS_FreeValue(ctx, global);
}

void performance_init(JSContext *ctx)
{
    JSClassDef d = { "Performance" };

    DCHECK(g_perf_slot < 0, "performance_init ran twice — the class, the slot and the two operations are "
                            "declared once per AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.5's check is a class-id comparison and a page cannot forge one. */
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
                      "HR-TIME §7 Performance's class — its per-realm prototype slot and Web IDL §3.7.5's "
                      "brand");
    agent_state_id("performance", &g_perf_slot,
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
       every Performance it minted would be branded with an id the live runtime never gave out. Safe here for
       that header's reason — the class has no finalizer and no gc_mark, so nothing dispatched through it runs
       after this column. */
    g_perf_class = 0;
    g_perf_slot = -1;
    g_id_now = -1;
    g_id_tojson = -1;
}
