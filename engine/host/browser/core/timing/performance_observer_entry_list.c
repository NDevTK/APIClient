/* PERFORMANCE TIMELINE §4.2.2 The PerformanceObserverEntryList interface, and §5.5 Filter buffer by name and
 * type — the algorithm its three members are the whole of. See the header for why the list is a JS Array and
 * why there is no constructor. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/timing/performance_entry.h"
#include "core/timing/performance_observer_entry_list.h"

static JSClassID g_class;
static JSValue   g_list_key = JS_UNDEFINED;   /* the entry-list slot's key, as a private Symbol */
static JSAtom    g_atom_list = JS_ATOM_NULL;
static int       g_id_get = -1, g_id_by_type = -1, g_id_by_name = -1;
static int       g_ready;

/* WHICH OF §4.2.2's THREE MEMBERS IS ASKING. All three are §5.5 with two of its three inputs decided by the
   member rather than by the page (§4.2.2.1 passes null and null, §4.2.2.2 null and the argument, §4.2.2.3 the
   argument and either null or the second argument), so they are ONE body under a magic and never three. */
typedef enum { PEL_ALL = 0, PEL_BY_TYPE, PEL_BY_NAME } PelMember;

/* THE OBJECT'S §4.2.2 ENTRY LIST. OWNED. */
static JSValue pel_list(JSContext *ctx, JSValueConst v)
{
    JSValue list;

    /* WEB IDL §3.7.6's BRAND TEST, AND IT IS A REAL TypeError RATHER THAN AN ASSERT. The receiver is
       PAGE-SUPPLIED INPUT — `PerformanceObserverEntryList.prototype.getEntries.call(null)` is one line a
       forcing solver writes constantly — so a DCHECK here would hand any page an abort switch for the engine.
       core/idl_args.h states the same rule for every other brand in this tree. */
    if (JS_GetClassID(v) != g_class)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (JS_GetOwnSlot(ctx, &list, v, g_atom_list) <= 0)
        list = JS_UNDEFINED;
    DCHECK(JS_IsArray(list),
           "a PerformanceObserverEntryList carries no entry list — §4.2.2 says its entry list \"is initialized "
           "upon construction\" and performance_observer_entry_list_new is the only mint, so one without it "
           "was built somewhere that is not this file");
    return list;
}

static uint32_t pel_len(JSContext *ctx, JSValueConst arr)
{
    JSValue v = JS_GetPropertyStr(ctx, (JSValue)arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* §5.5 STEP 2.1 / 2.2's "is not identical to" — over the value the entry actually holds.
 *
 * THE ENTRY'S OWN FIELDS ARE JSValues AND NOT C STRINGS (core/timing/performance_entry.h states why: a name
 * can be unknown external input and a C string would launder the taint), so the comparison is Web IDL's on the
 * values rather than a strcmp on two copies.
 *
 * NAMED RESIDUAL — AN ENTRY WHOSE `name` IS UNKNOWN EXTERNAL INPUT IS COMPARED CONCRETELY HERE.
 *   WHAT IS NOT COVERED. `performance.mark(location.hash)` stores a name this engine has not determined, and
 *     §5.5 step 2.2 then asks an identity question over it. This answers it with the interpreter's strict
 *     equality, which is FALSE for an unknown against any string — one world, chosen, where the run has
 *     observed nothing that decides it. `entryType` is not in this residual: every producer in this build
 *     mints it as a constant of its own standard (USER TIMING §2.2.1 step 4's "mark"), so step 2.1's operand
 *     is never unknown and the two steps are not the same case wearing one name.
 *   WHAT THE NEXT DIFF BUILDS. A filter that, where either operand is unknown, leaves BOTH worlds standing —
 *     the entry kept and the entry dropped — so the arm in which the page's `getEntriesByName` finds its entry
 *     is explored rather than decided here. What must exist afterward is a seam this algorithm can ASK at;
 *     there is none inside a plain C member body, which is the reason this is a residual and not an `if`.
 *   HOW ITS ABSENCE WOULD SHOW. A page that marks with an attacker-controlled name and then reads the marks
 *     back by name sees an empty list on every arm, and any endpoint it would have built out of that entry is
 *     never derived. */
static bool pel_identical(JSContext *ctx, JSValueConst want, JSValueConst have)
{
    return JS_IsStrictEqual(ctx, want, have);
}

/* §5.5's step 3: "Sort results's entries in chronological order with respect to startTime".
 *
 * INSERTION, SO IT IS STABLE, and stability is what carries the arrival order of two entries the sort cannot
 * separate. `n` here is one observer's buffer between two turns of the event loop, so the quadratic is over a
 * handful of entries and the alternative — a comparison function the page could observe running — is not one
 * a C activation may host at all.
 *
 * NAMED RESIDUAL — AN ENTRY WHOSE `startTime` IS NOT A CONCRETE NUMBER HOLDS ITS ARRIVAL POSITION.
 *   WHAT IS NOT COVERED. USER TIMING §2.2.1 step 5 takes `markOptions`'s `startTime` from the page, and
 *     CLAUDE.md §Solver-half's rule is that opacity SURVIVES numeric coercion — so a mark made with an unknown
 *     start time carries an unknown here. Two such entries, or one against a number, are not ordered.
 *   WHAT THE NEXT DIFF BUILDS. An ordering that treats an undecided comparison as a fork rather than as a tie,
 *     so both orders of the pair are explored; what must exist afterward is the same asking seam the name
 *     residual above names, reached from a sort rather than from a filter.
 *   HOW ITS ABSENCE WOULD SHOW. `performance.mark('a', {startTime: Number(location.hash.slice(1))})` followed
 *     by a concrete later mark yields one order from `getEntries()` where a browser's answer depends on the
 *     value, so a page that reads `entries[0].name` takes one arm and the other is never reached. */
static void pel_sort(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = pel_len(ctx, arr), i, j;

    for (i = 1; i < n; i++) {
        JSValue cur = JS_GetPropertyUint32(ctx, (JSValue)arr, i);
        PerfEntry *ce = performance_entry_of(cur);
        double ck;

        DCHECK(ce != NULL, "§5.5's sort met something that is not a PerformanceEntry — every writer of a "
                           "result list in this file appends what §5.5 step 2.3 appended, which is an entry");
        if (!JS_IsNumber(ce->start_time)) { JS_FreeValue(ctx, cur); continue; }
        JS_ToFloat64(ctx, &ck, ce->start_time);
        j = i;
        while (j > 0) {
            JSValue prev = JS_GetPropertyUint32(ctx, (JSValue)arr, j - 1);
            PerfEntry *pe = performance_entry_of(prev);
            double pk;
            bool swap;

            DCHECK(pe != NULL, "§5.5's sort met something that is not a PerformanceEntry");
            if (!JS_IsNumber(pe->start_time)) { JS_FreeValue(ctx, prev); break; }
            JS_ToFloat64(ctx, &pk, pe->start_time);
            swap = pk > ck;   /* STRICTLY greater, so equal start times keep their arrival order */
            if (!swap) { JS_FreeValue(ctx, prev); break; }
            JS_SetPropertyUint32(ctx, (JSValue)arr, j, prev);
            j--;
        }
        JS_SetPropertyUint32(ctx, (JSValue)arr, j, cur);
    }
}

/* §5.5 Filter buffer by name and type, over `buffer` with `name` and `type` — each either a real value or
   JS_NULL, which is this file's spelling of the standard's null. */
static JSValue pel_filter(JSContext *ctx, JSValueConst buffer, JSValueConst name, JSValueConst type)
{
    JSValue result = JS_NewArray(ctx);                                   /* step 1 */
    uint32_t n, i, out = 0;

    CHECK(!JS_IsException(result), "§5.5's result list could not be allocated");
    n = pel_len(ctx, buffer);
    for (i = 0; i < n; i++) {                                            /* step 2 */
        JSValue entry = JS_GetPropertyUint32(ctx, (JSValue)buffer, i);
        PerfEntry *e = performance_entry_of(entry);

        DCHECK(e != NULL, "a §4.2.2 entry list held something that is not a PerformanceEntry — §5.3 step 3.3.5 "
                          "builds one out of an observer buffer, and §5.1 step 7 is the only writer of that");
        if (!JS_IsNull(type) && !pel_identical(ctx, type, e->entry_type)) {   /* step 2.1 */
            JS_FreeValue(ctx, entry);
            continue;
        }
        if (!JS_IsNull(name) && !pel_identical(ctx, name, e->name)) {         /* step 2.2 */
            JS_FreeValue(ctx, entry);
            continue;
        }
        JS_SetPropertyUint32(ctx, result, out++, entry);                     /* step 2.3 */
    }
    pel_sort(ctx, result);                                               /* step 3 */
    return result;                                                       /* step 4 */
}

/* §4.2.2.1, §4.2.2.2 and §4.2.2.3 — one body, because all three are §5.5 with the member deciding which of its
   inputs are null. Each returns a `PerformanceEntryList`, which Web IDL renders as a JS Array. */
static JSValue js_pel_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue list = pel_list(ctx, this_val), r;
    JSValueConst name = JS_NULL, type = JS_NULL;

    if (JS_IsException(list)) return JS_EXCEPTION;
    switch ((PelMember)magic) {
    case PEL_ALL:
        break;
    case PEL_BY_TYPE:
        DCHECK(argc >= 1,
               "getEntriesByType reached its body without the `type` its declaration requires — §4.2.2.2's "
               "`getEntriesByType(DOMString type)` is a REQUIRED argument, so Web IDL's arity check is what "
               "makes a bare call a TypeError before this body runs");
        type = argv[0];
        break;
    case PEL_BY_NAME:
        DCHECK(argc >= 1,
               "getEntriesByName reached its body without the `name` its declaration requires — §4.2.2.3's "
               "first argument is REQUIRED and the declaration's arity check is what refuses a bare call");
        name = argv[0];
        /* §4.2.2.3: "type set to null if optional entryType is omitted, or set to the method's input type
           parameter otherwise". OMITTED is the question, so it is asked of the declaration rather than of the
           value: `getEntriesByName('a', undefined)` passed a value and §4.2.2.3 makes it the type. */
        if (idl_arg_given(argc, argv, 1)) type = argv[1];
        break;
    default:
        JS_FreeValue(ctx, list);
        DFAIL("a PerformanceObserverEntryList member ran with a magic §4.2.2 declares no member for — the magic "
              "IS the member, so an unknown one means a name was installed with no arm to answer it");
        return JS_UNDEFINED;
    }
    r = pel_filter(ctx, list, name, type);
    JS_FreeValue(ctx, list);
    return r;
}

JSValue performance_observer_entry_list_new(JSContext *ctx, JSValueConst entries)
{
    JSValue proto, obj;

    DCHECK(JS_IsArray(entries),
           "§5.3 step 3.3.5 was handed something that is not a list of entries — the only caller takes a copy "
           "of an observer buffer, which §5.1 step 7 builds as an Array");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "a PerformanceObserverEntryList was minted in a realm that never ran its install — §3.7 gives every "
           "realm its own prototype, and the page that reads this list is this realm's");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    /* §4.2.2's entry list, "initialized upon construction" — DEFINED as an own property rather than assigned,
       because an assignment is [[Set]] and would consult a prototype chain. See core/idl_slots.h. */
    JS_DefinePropertyValue(ctx, obj, g_atom_list, JS_DupValue(ctx, entries), 0);
    return obj;
}

void performance_observer_entry_list_init(JSContext *ctx)
{
    static const IdlArgType BY_TYPE_ARGS[1] = { IDL_DOMSTRING };
    static const IdlArgType BY_NAME_ARGS[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
    JSClassDef d = { "PerformanceObserverEntryList" };

    /* NOT `if (g_ready) return;` — this component has exactly ONE declaration site (its owner's `_init`), so
       the test could never be true and what it could do is hide a release that left the latch set. */
    DCHECK(!g_ready, "performance_observer_entry_list_init ran twice — §4.2.2's class is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);

    g_list_key = JS_NewSymbol(ctx, "performanceObserverEntryList", false);
    CHECK(!JS_IsException(g_list_key), "the §4.2.2 entry-list slot key could not be allocated");
    g_atom_list = JS_ValueToAtom(ctx, g_list_key);
    CHECK(g_atom_list != JS_ATOM_NULL, "the §4.2.2 entry-list slot key could not be interned");

    g_id_get     = idl_method_id(ctx, NULL, 0, js_pel_get, PEL_ALL);
    g_id_by_type = idl_method_id(ctx, BY_TYPE_ARGS, 1, js_pel_get, PEL_BY_TYPE);
    g_id_by_name = idl_method_id(ctx, BY_NAME_ARGS, 2, js_pel_get, PEL_BY_NAME);
    idl_optional_from(1);                /* `getEntriesByName(DOMString name, optional DOMString type)` */
    g_ready = 1;
}

void performance_observer_entry_list_install(JSContext *ctx)
{
    JSValue proto, prev, global;

    DCHECK(g_class != 0, "a realm asked for PerformanceObserverEntryList.prototype before the class existed");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "performance_observer_entry_list_install ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* §4.2.2's IDL inherits nothing, so this chains to %Object.prototype%. */
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "PerformanceObserverEntryList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PerformanceObserverEntryList");
    idl_install_method(ctx, proto, "getEntries", g_id_get);
    idl_install_method(ctx, proto, "getEntriesByType", g_id_by_type);
    idl_install_method(ctx, proto, "getEntriesByName", g_id_by_name);
    JS_SetClassProto(ctx, g_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT. §4.2.2 declares no constructor, so `new PerformanceObserverEntryList()` is a
       TypeError — and the NAME's presence is what a feature-detecting bundle reads, and what
       `entries instanceof PerformanceObserverEntryList` needs. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "PerformanceObserverEntryList",
                                         idl_interface_object(ctx, "PerformanceObserverEntryList", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void performance_observer_entry_list_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — the declare pass this pairs with is unconditional, so a release in an
       agent that never declared is the thing to CRASH on rather than the thing to skip. */
    DCHECK(g_ready, "§4.2.2 was released in an agent that never declared it");
    /* The prototypes and interface objects are the REALMS' and go with their contexts. The class id goes back
       to 0 because a class is registered in a RUNTIME and because it doubles as this file's init latch. */
    JS_FreeValueRT(rt, g_list_key);
    g_list_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_list);
    g_atom_list = JS_ATOM_NULL;
    g_ready = 0;
    g_class = 0;
    g_id_get = g_id_by_type = g_id_by_name = -1;
}
