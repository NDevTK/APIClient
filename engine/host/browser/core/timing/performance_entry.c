/* PERFORMANCE TIMELINE §3 The PerformanceEntry interface. See performance_entry.h for what is built, what is
   named as a residual, and why there is no performance entry buffer in this component. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/timing/performance_entry.h"
#include "solver/concolic.h"   /* an unknown NAME denotes its SHAPE — see concolic_name_cstr */
#include "solver/cow.h"

static JSClassID g_entry_class;
static int g_id_tojson = -1;   /* §3's `[Default] object toJSON()` */

/* WHICH OF §3's FOUR STATED ATTRIBUTES a read is — one getter body over the three that state the same
   sentence ("this attribute must return the value it is initialized to"), because three copies of one
   JS_DupValue could drift from each other and could not disagree in any interesting way. `duration` is NOT
   one of them: §3 gives it GETTER STEPS that compute, so it is its own body below. */
enum { PE_NAME = 0, PE_ENTRY_TYPE, PE_START_TIME };

PerfEntry *performance_entry_of(JSValueConst v)
{
    PerfEntry *e = g_entry_class ? JS_GetOpaque(v, g_entry_class) : NULL;

    /* THE CAPTURE IS IN THE ACCESSOR every member reaches, which is the rule for a component's own C record:
       a record a flow has REACHED is one it may write, the delta dedups to one entry, and there is then no
       write site left to miss. Every attribute §3 and its derived interfaces declare is READONLY and this
       engine writes an entry once at its mint, so today there is no write for the capture to catch — it is
       what makes that a fact nothing has to re-check the day a member is added. The layout is the DERIVED
       struct's, taken from the record itself, because the base has no way to know how much of an entry it is
       looking at; performance_entry_new is where the two are checked against each other. */
    if (e) cow_capture_host_record(v, e, e->cls->rec);
    return e;
}

bool performance_entry_is(JSValueConst v)
{
    return g_entry_class != 0 && JS_GetClassID(v) == g_entry_class;
}

bool performance_entry_is_a(JSValueConst v, const PerfEntryClass *cls)
{
    PerfEntry *e;

    DCHECK(cls != NULL, "a PerformanceEntry brand check was asked which DERIVED interface with no interface "
                        "named — the class record is the deriving component's own static and is the only "
                        "thing on an entry a page cannot forge");
    /* JS_GetOpaque and not performance_entry_of: a brand check is a QUESTION and must not capture, because it
       is asked of values that are not entries at all and of entries a flow is only inspecting. */
    e = g_entry_class ? JS_GetOpaque(v, g_entry_class) : NULL;
    return e != NULL && e->cls == cls;
}

JSValue performance_entry_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_entry_class);

    DCHECK(!JS_IsNull(proto), "PerformanceEntry.prototype was asked for in a realm that never ran its install "
                              "— a derived interface's prototype must chain to THIS realm's, and one built "
                              "over Object.prototype would answer §3's four attributes nowhere while Web IDL "
                              "§3.7.3's proto-step assertion at the derived tag is what would say so");
    return proto;
}

/* ---- the mint: §3's "initialize a PerformanceEntry" -------------------------------------------------------
 *
 * "To initialize a PerformanceEntry entry given a DOMHighResTimeStamp startTime, a DOMString entryType, a
 * DOMString name, and an optional DOMHighResTimeStamp endTime (default 0)" — five steps, of which step 1 is
 * an assertion and steps 2-5 are the four fields.
 *
 * THE OBJECT IS BUILT HERE AND NOT BY THE CALLER, which is what makes this the ONE mint rather than a helper.
 * §3 has no constructor of its own — a derived interface's does — so every entry in the platform reaches its
 * class, its opaque and its four fields through this line, and the invariants below are asked once for all of
 * them instead of once per deriving standard. */
JSValue performance_entry_new(JSContext *ctx, JSValueConst proto, const PerfEntryClass *cls, void *derived,
                              JSValue start_time, JSValue entry_type, JSValue name, double end_time)
{
    PerfEntry *e = (PerfEntry *)derived;
    JSValue obj;

    DCHECK(g_entry_class != 0, "a PerformanceEntry was minted before performance_entry_init declared §3's "
                               "class");
    DCHECK(cls != NULL && cls->rec != NULL && derived != NULL,
           "a PerformanceEntry was minted with no derived-interface record — §3 is a BASE interface and every "
           "entry a page sees is an instance of something derived from it, so the mint is reached with that "
           "interface's identity and COW layout or not at all");
    /* THE LAYOUT COVERS THE BASE, asserted rather than trusted, because the deriving component states it and a
       struct whose first member is a PerfEntry is the only shape that can be read back through this file. A
       layout smaller than the base would make the capture, the finalizer and the gc_mark disagree about which
       bytes belong to the record — the exact defect the one-list rule exists to make impossible. */
    DCHECK(cls->rec->size >= sizeof(PerfEntry),
           "a PerformanceEntry's derived record states a COW layout SMALLER than §3's own fields — the derived "
           "struct's first member must be a PerfEntry, and its layout must be the whole of it");
    /* STEP 1: "Assert: entryType is defined in the entry type registry." The registry is the Timing Entry
       Types Registry, and what THIS build mints is one of its rows — "mark", from User Timing §2.2. The
       assertion is written as the membership §3 states rather than as a list this file keeps: a second entry
       type arriving here without the standard that defines it is exactly what the spec's own assert is for.
       ONE CALL SITE REACHES IT TODAY (User Timing's PerformanceMark mint), so the abort names its remedy with
       nothing left implicit; the day this has many callers the site travels with the operation. */
    DCHECK(JS_IsString(entry_type),
           "a PerformanceEntry was minted with an entryType that is not a string — §3 step 1 asserts the type "
           "is defined in the Timing Entry Types Registry, so it is a constant of the standard that mints the "
           "entry and never a value a page reached this engine with");
    /* AND THE NAME MAY BE UNKNOWN EXTERNAL INPUT, WHICH IS NOT THE SAME QUESTION. `performance.mark(cfg.id)`
       is a page's ordinary spelling, `DOMString` is idl_concolic_rule's CROSSES default, and a DCHECK may only
       ever stand on a value THIS codebase computed — so the assert here is about the two SHAPES the platform
       can produce and never about the bytes. */
    DCHECK(JS_IsString(name) || concolic_is(name),
           "a PerformanceEntry's name reached the mint as neither a string nor unknown external input — every "
           "door onto this operation declares it `DOMString`, so core/idl_args.h's conversion produces one of "
           "those two and a third shape means this entry was minted from a path that never ran a declaration");
    DCHECK(JS_IsNumber(start_time) || concolic_is(start_time),
           "a PerformanceEntry's startTime reached the mint as neither a Number nor unknown external input — "
           "§3 declares it DOMHighResTimeStamp, which is a `double`");

    obj = JS_NewObjectProtoClass(ctx, proto, g_entry_class);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, start_time);
        JS_FreeValue(ctx, entry_type);
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    /* STEPS 2-5, IN §3's OWN ORDER. The record is COMPLETE before it is attached and nothing between the
       allocation above and the JS_SetOpaque below can collect, which is why the finalizer asserts the record
       is there rather than reading past its absence. */
    e->cls = cls;
    e->start_time = start_time;   /* step 2 */
    e->entry_type = entry_type;   /* step 3 */
    e->name = name;               /* step 4 */
    e->end_time = end_time;       /* step 5 */
    JS_SetOpaque(obj, e);
    return obj;
}

static void entry_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    PerfEntry *e = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!e) return;`. performance_entry_new is the ONE mint and it attaches a complete record with
       nothing in between that allocates on the JS heap. JS_GetAnyOpaque because the collector dispatched here
       THROUGH the class, and this release column runs after core/platform.c's — see core/agent_state.h. */
    DCHECK(e != NULL, "a PerformanceEntry was finalized with no record — §3 has exactly one mint and it "
                      "attaches the record with nothing in between that could collect");
    /* THE DERIVED INTERFACE'S FIELDS FIRST, then §3's — the reverse of the order they were placed in, and the
       list on each side is the one its own COW layout names. A deriving component that frees §3's here would
       double-free; one that forgets its own leaks, which is why `rec` states the whole struct and this pair is
       read together. */
    if (e->cls->release) e->cls->release(rt, e);
    JS_FreeValueRT(rt, e->name);
    JS_FreeValueRT(rt, e->entry_type);
    JS_FreeValueRT(rt, e->start_time);
    free(e);
}

static void entry_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    PerfEntry *e = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(e != NULL, "a PerformanceEntry was marked with no record — its fields are counted references and an "
                      "unmarked child is read by gc_scan as rooted from outside the heap");
    if (e->cls->mark) e->cls->mark(rt, e, mark_func);
    JS_MarkValue(rt, e->name, mark_func);
    JS_MarkValue(rt, e->entry_type, mark_func);
    JS_MarkValue(rt, e->start_time, mark_func);
}

/* ---- §3's attributes -------------------------------------------------------------------------------------
 *
 * WEB IDL §3.7.6 Attributes' BRAND, stated in the getter because a plain-C getter (idl_args.h: IdlGetter) has
 * nowhere else to put it and because an attribute's getter has nothing to convert, so there is no ordering
 * hazard. A real TypeError and not an assert: a feature detector that pulls the descriptor and applies the
 * getter to a bare object reads the throw as "this is a real interface", and tells it apart from `undefined`. */
static PerfEntry *entry_here(JSContext *ctx, JSValueConst v)
{
    PerfEntry *e = performance_entry_of(v);

    if (!e) {
        JS_ThrowTypeError(ctx, "a PerformanceEntry member was reached on something that is not a "
                               "PerformanceEntry");
        return NULL;
    }
    return e;
}

/* §3's `name`: "This attribute must return the value it is initialized to." Likewise `entryType` and
   `startTime`. One body, chosen by the magic the install states. */
static JSValue js_entry_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    PerfEntry *e = entry_here(ctx, this_val);

    if (!e) return JS_EXCEPTION;
    switch (magic) {
    case PE_NAME:       return JS_DupValue(ctx, e->name);
    case PE_ENTRY_TYPE: return JS_DupValue(ctx, e->entry_type);
    case PE_START_TIME: return JS_DupValue(ctx, e->start_time);
    default: break;
    }
    DFAIL("a PerformanceEntry attribute was installed with a magic naming none of §3's three "
          "return-what-it-was-initialized-to attributes — `duration` is not one of them, because §3 gives it "
          "GETTER STEPS that compute and it has its own body");
    return JS_UNDEFINED;
}

/* §3's `duration`: "The getter steps for the duration attribute are to return 0 if this's end time is 0;
   otherwise this's end time - this's startTime."
   THE SECOND ARM IS UNREACHABLE IN THIS BUILD AND IS WRITTEN ANYWAY, because it is one line of the standard
   and because the alternative — a getter that returns 0 under an assert — would hide which of the two facts
   is doing the work. User Timing §2.2 states the same answer from the other side for the one entry type this
   build mints ("The duration attribute must return a DOMHighResTimeStamp of value 0"), and it holds here
   BECAUSE the mark's end time is 0, not instead of it. */
static JSValue js_entry_get_duration(JSContext *ctx, JSValueConst this_val, int magic)
{
    PerfEntry *e = entry_here(ctx, this_val);
    double start = 0;

    (void)magic;
    if (!e) return JS_EXCEPTION;
    if (e->end_time == 0) return JS_NewFloat64(ctx, 0);
    /* An end time this engine WROTE, so the arithmetic needs a real startTime. The one interface that will
       reach here is User Timing §2.3's PerformanceMeasure, whose §2.1.3 measure() resolves both ends through
       §3.1 Convert a mark to a timestamp before the entry exists — so a startTime that is still unknown at
       this line means that resolution did not happen. */
    DCHECK(JS_IsNumber(e->start_time),
           "§3's duration getter reached its subtracting arm over a startTime that is unknown external input "
           "— the interface that gives an entry a non-zero end time resolves both ends to real timestamps "
           "before minting it, so an unknown here means the entry was minted without that resolution");
    JS_ToFloat64(ctx, &start, e->start_time);
    return JS_NewFloat64(ctx, e->end_time - start);
}

/* §3's `[Default] object toJSON()` — "When toJSON is called, run [WEBIDL]'s default toJSON steps."
 *
 * WEB IDL §3.7.7.1.1 Default toJSON operation: build an ordered map by walking the inheritance stack
 * base-first, and on each interface — "if a toJSON operation with a [Default] extended attribute is declared
 * on I" — take each exposed regular attribute in order whose value is a JSON type.
 *
 * WHICH IS FOUR KEYS HERE, AND EVERY PART OF THAT COUNT IS A FACT ABOUT THIS BUILD RATHER THAN A SHORTCUT.
 * §3 declares the `[Default]` toJSON, so §3's attributes are the ones taken; `id` and `navigationId` are not
 * installed (performance_entry.h says why, and what the next diff builds), so they are not exposed and are
 * not among them. NOTHING A DERIVED INTERFACE ADDS APPEARS EITHER, and that is the standard's answer and not
 * this file's: User Timing §2.2's PerformanceMark declares NO toJSON of its own, so §3.7.7.1.1's condition
 * excludes its members before their types are asked — and `detail` would fail the type test in any case,
 * since Web IDL §2.5.3.1 toJSON's list of JSON types does not contain `any`.
 * THE MAP IS BUILT FROM THE GETTERS and not from the record: "Let value be the result of running the getter
 * steps of attr with object as this", so the object cannot disagree with the attributes. */
static JSValue js_entry_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const struct { const char *key; IdlGetter get; int magic; } J[] = {
        { "name",      js_entry_get,          PE_NAME },
        { "entryType", js_entry_get,          PE_ENTRY_TYPE },
        { "startTime", js_entry_get,          PE_START_TIME },
        { "duration",  js_entry_get_duration, 0 },
    };
    JSValue out;
    size_t i;

    (void)argc; (void)argv; (void)magic;
    DCHECK(performance_entry_is(this_val),
           "§3's toJSON() ran on a receiver that is not a PerformanceEntry — the declaration states Web IDL "
           "§3.7 Interfaces' implementation check, so reaching the body means idl_implementation_check did "
           "not run for it");
    /* §3.7.7.1.1 step 4's OrdinaryObjectCreate(%Object.prototype%) — a plain object of THIS realm. */
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a PerformanceEntry's toJSON result could not be allocated");
    for (i = 0; i < sizeof J / sizeof J[0]; i++) {
        JSValue v = J[i].get(ctx, this_val, J[i].magic);

        if (JS_IsException(v)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        JS_SetPropertyStr(ctx, out, J[i].key, v);
    }
    return out;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void performance_entry_install(JSContext *ctx)
{
    JSValue proto, prev, global;

    prev = JS_GetClassProto(ctx, g_entry_class);
    DCHECK(JS_IsNull(prev), "performance_entry_install ran twice in one realm — everything already holding "
                            "the first PerformanceEntry.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* §3's IDL inherits nothing, so this chains to %Object.prototype% — which is what Web IDL §3.7.3's
       proto-step assertion inside idl_interface_tag checks against browser/idl_inheritance.h. */
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "PerformanceEntry.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PerformanceEntry");
    /* IN §3's OWN IDL ORDER, minus the two performance_entry.h names as absent. Every one of these getters
       reaches none of the page's code — each is a JS_DupValue or a double — so each declares that, which is
       what lets a C reader of the property go through the trampoline instead of aborting. */
    idl_install_accessor_no_user_code(ctx, proto, "name", js_entry_get, PE_NAME, -1);
    idl_install_accessor_no_user_code(ctx, proto, "entryType", js_entry_get, PE_ENTRY_TYPE, -1);
    idl_install_accessor_no_user_code(ctx, proto, "startTime", js_entry_get, PE_START_TIME, -1);
    idl_install_accessor_no_user_code(ctx, proto, "duration", js_entry_get_duration, 0, -1);
    idl_install_method(ctx, proto, "toJSON", g_id_tojson);
    JS_SetClassProto(ctx, g_entry_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. §3 declares no constructor, so `new
       PerformanceEntry()` is a TypeError — and its PRESENCE is what a feature-detecting bundle reads, and
       what `entry instanceof PerformanceEntry` needs. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "PerformanceEntry",
                      idl_interface_object(ctx, "PerformanceEntry", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void performance_entry_init(JSContext *ctx)
{
    JSClassDef d = { "PerformanceEntry", entry_finalizer, entry_gc_mark };

    DCHECK(g_entry_class == 0, "performance_entry_init ran twice — §3's class is declared once per AGENT, and "
                               "a second class id would leave every entry already minted branded with the "
                               "first");
    JS_NewClassID(JS_GetRuntime(ctx), &g_entry_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_entry_class, &d) == 0,
          "PerformanceEntry: the class could not be declared");
    /* `toJSON` takes no arguments, so the declaration has no type list; what it states is Web IDL §3.7
       Interfaces' implementation-check an object, step 3, which §3.7.7 Operations asks BEFORE argument
       conversion. A plain C body and not a step machine: it runs four of this file's own getters over stored
       values and reaches none of the page's code, so there is nothing in it to suspend at. */
    g_id_tojson = idl_method_id(ctx, NULL, 0, js_entry_tojson, 0);
    idl_this_iface(performance_entry_is, "PerformanceEntry");
    agent_state_class("performance_entry", &g_entry_class,
                      "PERFORMANCE TIMELINE §3's PerformanceEntry class — the opaque every derived entry "
                      "wears, and this component's declaration latch");
    agent_state_id("performance_entry", &g_id_tojson, "§3's toJSON() declaration");
    realm_declare_intrinsic(performance_entry_install);
}

void performance_entry_free(void)
{
    /* The prototypes and the interface objects are the REALMS' and go with their contexts; an entry's record
       is released by the finalizer, which runs AFTER this column and therefore reads none of these — see
       core/agent_state.h. The class id goes back to 0 because a class is registered in a RUNTIME and because
       it doubles as this file's own init latch: carrying it would make a second agent's performance_entry_init
       return before re-registering the class, and every entry that agent minted would be branded with an id
       the live runtime never gave out. */
    g_entry_class = 0;
    g_id_tojson = -1;
}
