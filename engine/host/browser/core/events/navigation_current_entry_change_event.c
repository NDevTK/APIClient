/* THE NavigationCurrentEntryChangeEvent INTERFACE — HTML §7.2.7.1.
 *
 *     [Exposed=Window]
 *     interface NavigationCurrentEntryChangeEvent : Event {
 *       constructor(DOMString type, NavigationCurrentEntryChangeEventInit eventInitDict);
 *       readonly attribute NavigationType? navigationType;
 *       readonly attribute NavigationHistoryEntry from;
 *     };
 *     dictionary NavigationCurrentEntryChangeEventInit : EventInit {
 *       NavigationType? navigationType = null;
 *       required NavigationHistoryEntry from;
 *     };
 *
 * IT IS THE navigation API'S ONLY NOTIFICATION THAT THE CURRENT ENTRY MOVED, and both attributes are branched
 * on rather than logged. §7.2.6.4 fires it for every same-document push, replace and traverse, and §7.2.6.6's
 * `updateCurrentEntry` fires it with a NULL navigationType — which is the whole of how a listener tells "we
 * navigated" from "the entry we are already on had its state rewritten". A handler that read `undefined` for
 * that attribute would take the second arm for every navigation.
 *
 * `from` IS REQUIRED AND NON-NULLABLE, which is the one thing the constructor's declaration has to enforce:
 * `new NavigationCurrentEntryChangeEvent("currententrychange")` is a TypeError (the dictionary itself is a
 * required argument), and so is one whose `from` is absent or is not a NavigationHistoryEntry. All three come
 * off the DECLARATION — the arity, the `required` flag and the interface brand — so this file's body performs
 * no check of its own.
 *
 * THE ENGINE'S OWN `from` IS THE PREVIOUS CURRENT ENTRY, NOT A COPY OF IT. §7.2.7.1's own note is why that
 * matters: "If navigationType is null or 'reload', then this value will be the same as
 * navigation.currentEntry" — a page compares the two objects, so the event carries the NavigationHistoryEntry
 * itself and a fresh wrapper over the same session history entry would answer that comparison wrongly.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's contents time-travel with the flow that fired it,
 * and the symbol is a brand a page cannot forge.
 *
 * THERE IS NO createEvent ROW: DOM §4.5's table does not name this interface, so
 * `document.createEvent('NavigationCurrentEntryChangeEvent')` is step 3's NotSupportedError. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/navigation_current_entry_change_event.h"
#include "core/frame/navigation_history_entry.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_nce_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

/* §7.2.6.3's `enum NavigationType { "push", "replace", "reload", "traverse" }` — the TYPE of the
   `navigationType` member, so the list is what the declaration carries and no body re-states it. */
static const char *const NAVIGATION_TYPE[] = { "push", "replace", "reload", "traverse", NULL };

static JSValue nce_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_nce_class);

    DCHECK(!JS_IsNull(proto), "NavigationCurrentEntryChangeEvent.prototype was asked for in a realm that "
                              "never ran its per-realm install");
    return proto;   /* OWNED */
}

/* "The navigationType and from attributes must return the values they were initialized to", read out of the
   one slot record. `magic` IS the member, so there is one body rather than two copies of the brand check. */
enum { NCE_NAVIGATION_TYPE = 0, NCE_FROM };
static const char *const NCE_SLOT_NAME[] = { "navigationType", "from" };

static JSValue js_nce_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a NavigationCurrentEntryChangeEvent attribute was read before its init ran");
    DCHECK(magic == NCE_NAVIGATION_TYPE || magic == NCE_FROM,
           "a NavigationCurrentEntryChangeEvent accessor was installed with a magic this interface has no "
           "member for");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a NavigationCurrentEntryChangeEvent attribute was read on something "
                                      "that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a NavigationCurrentEntryChangeEvent attribute was read on something "
                                      "that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, NCE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The two own slots, placed on an event whose Event half is already built. Both values are DUPPED: `from` is
   the very NavigationHistoryEntry the Navigation's entry list holds, because §7.2.7.1's note has a page
   comparing it against `navigation.currentEntry`. Returns -1 with the throw live. */
static int nce_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst navigation_type, JSValueConst from)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "navigationType", JS_DupValue(ctx, navigation_type));
    JS_SetPropertyStr(ctx, slots, "from", JS_DupValue(ctx, from));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue navigation_current_entry_change_event_new_to_fire(JSContext *ctx, const char *navigation_type,
                                                          JSValueConst from)
{
    JSValue tv, ev, nt;

    DCHECK(g_ready, "a currententrychange event was fired before its interface was declared — HTML §7.2.6.4 "
                    "fires it USING NavigationCurrentEntryChangeEvent, so the interface has to exist before "
                    "the entry list can be updated at all");
    DCHECK(JS_GetClassID(from) == navigation_history_entry_class(),
           "§7.2.7.1's `from` was initialized with something that is not a NavigationHistoryEntry — the IDL "
           "types it `required NavigationHistoryEntry`, and §7.2.6.4 passes the entry that was current");
    nt = navigation_type ? JS_NewString(ctx, navigation_type) : JS_NULL;
    if (JS_IsException(nt)) return nt;
    tv = JS_NewString(ctx, "currententrychange");
    if (JS_IsException(tv)) { JS_FreeValue(ctx, nt); return tv; }
    /* DOM's fire-an-event with none of the three flags set, because §7.2.6.4 sets none of them. It is TRUSTED:
       the user agent fired it, which is the difference a page reads off `isTrusted`. */
    ev = event_new_derived(ctx, nce_proto(ctx), tv, /*bubbles*/ false, /*cancelable*/ false,
                           /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (!JS_IsException(ev) && nce_init_slots(ctx, ev, nt, from) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, nt);
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, NavigationCurrentEntryChangeEventInit eventInitDict)` — and the dictionary is
 * NOT optional, which is what the arity of 2 states and what makes a one-argument construction a TypeError
 * before this body runs. That is unusual among the event interfaces and it follows from `required
 * NavigationHistoryEntry from`: a dictionary with a required member has no `= {}` to default to.
 *
 * The member list is in Web IDL's conversion order — the INHERITED EventInit members first, then this
 * dictionary's own lexicographically, which puts `from` before `navigationType`. That order is observable: a
 * page that throws from one member's getter pins which of them was read first. */
static const IdlArgType NCE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember NCE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "from", IDL_INTERFACE, /*required*/ true, NULL, 1 },
    { "navigationType", IDL_ENUM_NULLABLE, false, NAVIGATION_TYPE, 1, NULL, IDL_DEFAULT_NULL, NULL },
};

static JSValue js_nce_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue nt, from, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor NavigationCurrentEntryChangeEvent requires 'new'");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "NavigationCurrentEntryChangeEvent constructor requires a type and an "
                                      "eventInitDict");
    nt = idl_dict_get(ctx, init, "navigationType");
    from = idl_dict_get(ctx, init, "from");
    /* THE BRAND REFUSES A WRONG VALUE BEFORE ANY BODY IS ENTERED AND DOES NOT SEE AN UNKNOWN ONE. §3.2.15
       Interface types' test is IDL_INTERFACE's arm, reached through the class idl_iface_brand states for this
       declaration, and the §3.2.17 Dictionary types member loop rewrites a CONCOLIC member's type to IDL_ANY
       before that arm is asked — so `{from: <unknown>}` is the one shape that reaches here having passed no
       brand at all, and a message about a value that is "something else" misnames it.
       THE REFUSAL IS INTERMEDIATE AND THE FORK IT IS OWED NEEDS AN OBJECT THIS FILE CANNOT MINT. Over an
       unknown both of §3.2.15's steps are feasible, so the worlds are step 2's "Throw a TypeError." and a
       construction whose `from` slot holds a NavigationHistoryEntry — and §7.2.7.1's own note is what the
       second arm would have to satisfy ("If navigationType is null or 'reload', then this value will be the
       same as navigation.currentEntry"), a page comparing the slot against a live entry by IDENTITY. An
       unknown cannot answer that comparison, so the arm wants an entry rather than a value, which is not a
       fork this constructor can ask for on its own. */
    IDL_DCHECK_MEMBER(JS_GetClassID(from) == navigation_history_entry_class(), from, "from",
                      "`required NavigationHistoryEntry from` by HTML §7.2.7.1 The "
                      "NavigationCurrentEntryChangeEvent interface — branded by idl_iface_brand over "
                      "IDL_INTERFACE");
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, nce_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) && nce_init_slots(ctx, ev, nt, from) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, from);
    JS_FreeValue(ctx, nt);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_nce_proto[] = {
    JS_CGETSET_MAGIC_DEF("navigationType", js_nce_get, NULL, NCE_NAVIGATION_TYPE),
    JS_CGETSET_MAGIC_DEF("from", js_nce_get, NULL, NCE_FROM),
};

void navigation_current_entry_change_event_init(JSContext *ctx)
{
    JSClassDef d = { "NavigationCurrentEntryChangeEvent" };

    DCHECK(!g_ready, "navigation_current_entry_change_event_init ran twice — the interface is declared once "
                     "per AGENT");
    g_key = JS_NewSymbol(ctx, "navigationCurrentEntryChangeEventSlots", false);
    CHECK(!JS_IsException(g_key), "the NavigationCurrentEntryChangeEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nce_class);
    JS_NewClass(JS_GetRuntime(ctx), g_nce_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, NCE_CTOR_ARGS, 2, NCE_INIT,
                                       (int)(sizeof(NCE_INIT) / sizeof(NCE_INIT[0])), js_nce_ctor, 0);
    /* The other half of `required NavigationHistoryEntry from`: the CLASS the interface arm brands against.
       navigation_history_entry_init has to have run, which core/platform.c's declaration order gives it. */
    idl_iface_brand(navigation_history_entry_class());
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "HTML §7.2.7.1 The NavigationCurrentEntryChangeEvent interface's declaration latch");
    agent_state_class("event", &g_nce_class,
                      "HTML §7.2.7.1 The NavigationCurrentEntryChangeEvent interface's class, held for its "
                      "per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol HTML §7.2.7.1 The NavigationCurrentEntryChangeEvent interface's slot "
                      "record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "HTML §7.2.7.1 The NavigationCurrentEntryChangeEvent interface's `constructor(DOMString type, "
                   "NavigationCurrentEntryChangeEventInit eventInitDict)`");
    realm_declare_intrinsic(navigation_current_entry_change_event_install_protos);
}

void navigation_current_entry_change_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for NavigationCurrentEntryChangeEvent before its init declared it");
    prev = JS_GetClassProto(ctx, g_nce_class);
    DCHECK(JS_IsNull(prev), "navigation_current_entry_change_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "NavigationCurrentEntryChangeEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "NavigationCurrentEntryChangeEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_nce_proto,
                               (int)(sizeof(js_nce_proto) / sizeof(js_nce_proto[0])));
    JS_SetClassProto(ctx, g_nce_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one per realm, whose `prototype` is the prototype
       this same install just built. Its LENGTH is 2: Web IDL §3.7.4.1's length is the number of REQUIRED
       arguments, and this constructor's dictionary is not optional. */
    ctor = idl_step_constructor(ctx, "NavigationCurrentEntryChangeEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the NavigationCurrentEntryChangeEvent interface object could not be "
                                 "allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "NavigationCurrentEntryChangeEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void navigation_current_entry_change_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "HTML §7.2.7.1 The NavigationCurrentEntryChangeEvent interface was released in an agent that "
                    "never declared it — event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ctor_stepid = -1;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_nce_class = 0;
}
