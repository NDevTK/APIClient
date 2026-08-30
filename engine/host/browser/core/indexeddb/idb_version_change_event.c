/* INDEXED DATABASE §4.2's IDBVersionChangeEvent — the event the whole database half of the standard reports
 * its progress through, and §4.2's FIRE A VERSION CHANGE EVENT over it.
 *
 *     [Exposed=(Window,Worker)]
 *     interface IDBVersionChangeEvent : Event {
 *       constructor(DOMString type, optional IDBVersionChangeEventInit eventInitDict = {});
 *       readonly attribute unsigned long long oldVersion;
 *       readonly attribute unsigned long long? newVersion;
 *     };
 *     dictionary IDBVersionChangeEventInit : EventInit {
 *       unsigned long long oldVersion = 0;
 *       unsigned long long? newVersion = null;
 *     };
 *
 * WHY IT IS THE FIRST THING §5.1 NEEDS. Three of that algorithm's events are this interface — `versionchange`
 * at every other open connection, `blocked` at the request, and §5.7's `upgradeneeded` — and a page BRANCHES on
 * the two numbers: `if (e.oldVersion < 2) store = db.createObjectStore(...)` is the shape every migration in
 * every bundle is written in, so an `upgradeneeded` carrying a plain Event would run the wrong migration arm or
 * none. `newVersion` being NULLABLE is not decoration either: null is what a DELETE reports, which is the only
 * thing that tells `deleteDatabase`'s success event from `open`'s.
 *
 * THE FIRE IS A REQUEST AND NOT A CALL, for the reason every dispatch in this component is one: step 6 runs the
 * page's listeners, and a listener may loop, await, place a request against the upgrade transaction, or throw.
 * A C body that dispatched from a `JS_Call` would be the drive-to-completion this engine aborts on. So the
 * algorithm is stated as a run-helper the CALLING step machine rests inside, and its return value — §4.2 step
 * 7's legacyOutputDidListenersThrowFlag — is handed back as an out-parameter because §5.7 step 9.6 is a real
 * caller of it and the other two discard it.
 *
 * IT IS A REAL SUBCLASS, like PageTransitionEvent: `IDBVersionChangeEvent.prototype.__proto__ === Event.
 * prototype`, so `e instanceof Event` holds and the base half is event_new_derived's. The two slots hang off a
 * private Symbol so they are ordinary property writes the per-flow COW delta captures, and the interface object
 * and prototype are PER-REALM intrinsics — §3.7's rule, which in this engine decides ANSWERS and not just
 * identities, because a C member runs in the realm that defined it. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_version_change_event.h"
#include "core/realm.h"

#define VCE_OLD "oldVersion"
#define VCE_NEW "newVersion"   /* the number, or JS_NULL for the IDL's null */

static JSValue   g_key;
static JSClassID g_vce_class;
static int       g_ready;
static JSRuntime *g_vce_rt;
static int       g_ctor_stepid = -1;

static JSValue vce_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_vce_class);

    DCHECK(!JS_IsNull(proto),
           "IDBVersionChangeEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* The event's own slots, or UNDEFINED for a value that is not one. An OWN SLOT read — never a lookup, which
   would mint a concolic for an internal slot at the solver's absent-state seam. OWNED. */
static JSValue vce_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBVersionChangeEvent slot was asked for before its interface was declared");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBVersionChangeEvent slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, ev, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

/* §4.2 steps 4-5's two initialisers, placed on an event whose Event half is already built. `new_version` is
   CONSUMED. Returns -1 with the throw live. */
static int vce_init_slots(JSContext *ctx, JSValueConst ev, double old_version, JSValue new_version)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k;

    if (JS_IsException(slots)) {
        JS_FreeValue(ctx, new_version);
        return -1;
    }
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBVersionChangeEvent slot key could not be interned");
    JS_SetPropertyStr(ctx, slots, VCE_OLD, JS_NewFloat64(ctx, old_version));
    JS_SetPropertyStr(ctx, slots, VCE_NEW, new_version);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* §4.2 steps 1-5, as one construction: the event, its type, its two FALSE flags, and its two numbers. It is
   TRUSTED — every caller of this algorithm is the user agent. OWNED, or JS_EXCEPTION. */
static JSValue vce_new(JSContext *ctx, const char *type, double old_version, double new_version, bool has_new)
{
    JSValue tv, ev, proto;

    DCHECK(g_ready, "a version change event was fired before idb_version_change_event_init declared the "
                    "interface — §4.2 fires it USING IDBVersionChangeEvent, so the interface has to exist "
                    "before §5.1 can");
    DCHECK(type != NULL && *type, "fire a version change event was given no name — the algorithm is "
                                  "named-by-its-caller (§5.1's `versionchange` and `blocked`, §5.7's "
                                  "`upgradeneeded`) and there is no default");
    tv = JS_NewString(ctx, type);
    if (JS_IsException(tv))
        return tv;
    proto = vce_proto(ctx);
    /* "Set event's bubbles and cancelable attributes to false." Both, which is what makes a `versionchange`
       handler unable to cancel the upgrade it is being warned about — the only thing it can do is close(). */
    ev = event_new_derived(ctx, proto, tv, /*bubbles*/ false, /*cancelable*/ false, /*composed*/ false,
                           /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    if (vce_init_slots(ctx, ev, old_version, has_new ? JS_NewFloat64(ctx, new_version) : JS_NULL) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

int idb_fire_version_change_event_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap,
                                      JSValueConst target, const char *type, double old_version,
                                      double new_version, bool has_new, JSValue *pev, JSValue in,
                                      bool *pdid_throw, JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(pev != NULL, "fire a version change event was given no event slot — §4.2's step 1 creates the "
                        "event and step 6 dispatches it, and the caller holds it across the suspension "
                        "between them");
    /* §4.2 steps 1-5, ON THE FIRST ENTRY ONLY. A resume re-enters this helper with the dispatch already in
       flight, and minting a second event there would dispatch an event nobody was listening to the first of. */
    if (JS_IsUndefined(*pev)) {
        *pev = vce_new(ctx, type, old_version, new_version, has_new);
        if (JS_IsException(*pev)) {
            *pev = JS_UNDEFINED;
            JS_FreeValue(ctx, in);
            return -1;
        }
    }
    /* §4.2 step 6: "Dispatch event at target with legacyOutputDidListenersThrowFlag." */
    r = event_target_fire_run(ctx, phase, cb, cb_cap, target, *pev, JS_UNDEFINED, in, NULL, out_cb, out_argc);
    if (r != 0)
        return r;
    /* §4.2 step 7: "Return legacyOutputDidListenersThrowFlag." The flag is the DISPATCH's, recorded on the
       event by §2.9 itself, so it is read off the event rather than counted here. */
    if (pdid_throw)
        *pdid_throw = event_listeners_threw(ctx, *pev);
    return 0;
}

/* ---- §4.2's two attributes ------------------------------------------------------------------------------- */

static JSValue vce_field(JSContext *ctx, JSValueConst this_val, const char *field)
{
    JSValue slots = vce_slots(ctx, this_val), v;

    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "an IDBVersionChangeEvent attribute was read on something that is not "
                                      "an IDBVersionChangeEvent");
    }
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

/* "The oldVersion getter steps are to return the value it was initialized to." */
static JSValue js_vce_get_old(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return vce_field(ctx, this_val, VCE_OLD);
}

/* "The newVersion getter steps are to return the value it was initialized to ... or null if the database is
   being deleted." The slot holds the null itself, so the nullable type is what was stored rather than a hole
   this getter fills. */
static JSValue js_vce_get_new(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return vce_field(ctx, this_val, VCE_NEW);
}

/* ---- the constructor -------------------------------------------------------------------------------------
 *
 * IDBVersionChangeEventInit INHERITS EventInit, and Web IDL reads the INHERITED members first and each level
 * lexicographically among itself — bubbles, cancelable, composed, then newVersion, oldVersion. A single sorted
 * list would read `newVersion` before `bubbles`, which a page pins by throwing from a member's getter. */
static const IdlArgType VCE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember VCE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    /* `unsigned long long? newVersion = null` — the union of a number and the IDL null, so the declaration
       states the DEFAULT and the body reads whichever of the two the conversion left. */
    { "newVersion", IDL_ANY, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
    { "oldVersion", IDL_UNSIGNED_LONG_LONG, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
};

static JSValue js_vce_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    /* `optional IDBVersionChangeEventInit eventInitDict = {}` — the dictionary exists whether or not the page
       wrote one, so it is read out of the vector rather than reconstructed from `argc`. */
    JSValueConst init = argv[1];
    JSValue ev, old, nv, proto;
    double oldv = 0;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor IDBVersionChangeEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "IDBVersionChangeEvent constructor requires a type");
    DCHECK(JS_IsObject(init), "the IDBVersionChangeEvent constructor was handed no init dictionary — its IDL "
                              "writes `= {}`, so the conversion builds one carrying every declared default");
    old = idl_dict_get(ctx, init, "oldVersion");
    DCHECK(!JS_IsUndefined(old), "IDBVersionChangeEventInit's `oldVersion` is declared with the IDL's own "
                                 "`= 0`, so the conversion places it whether or not the page wrote one — an "
                                 "undefined here is the declaration having lost its default");
    if (JS_ToFloat64(ctx, &oldv, old) < 0) { JS_FreeValue(ctx, old); return JS_EXCEPTION; }
    JS_FreeValue(ctx, old);
    nv = idl_dict_get(ctx, init, "newVersion");
    /* The member is declared IDL_ANY over the union's two arms, so the numeric arm is converted here: §3.2.9's
       `unsigned long long` over whatever the page wrote, and the null arm passes straight through. */
    if (!JS_IsNull(nv)) {
        double d = 0;

        if (JS_ToFloat64(ctx, &d, nv) < 0) { JS_FreeValue(ctx, nv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, nv);
        nv = JS_NewFloat64(ctx, idl_unsigned_long_long_of(d));
    }
    proto = vce_proto(ctx);
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, proto, argv[0], idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"), idl_dict_bool(ctx, init, "composed"),
                           /*trusted*/ false);
    if (JS_IsException(ev)) {
        JS_FreeValue(ctx, nv);
        return ev;
    }
    if (vce_init_slots(ctx, ev, idl_unsigned_long_long_of(oldv), nv) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- install ---------------------------------------------------------------------------------------------- */

static const JSCFunctionListEntry js_vce_proto[] = {
    JS_CGETSET_MAGIC_DEF("oldVersion", js_vce_get_old, NULL, 0),
    JS_CGETSET_MAGIC_DEF("newVersion", js_vce_get_new, NULL, 0),
};

static void idb_version_change_event_install_realm(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for IDBVersionChangeEvent before its interface was declared");
    prev = JS_GetClassProto(ctx, g_vce_class);
    DCHECK(JS_IsNull(prev), "idb_version_change_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "IDBVersionChangeEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBVersionChangeEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_vce_proto, (int)(sizeof js_vce_proto / sizeof js_vce_proto[0]));
    JS_SetClassProto(ctx, g_vce_class, JS_DupValue(ctx, proto));

    ctor = idl_step_constructor(ctx, "IDBVersionChangeEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the IDBVersionChangeEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBVersionChangeEvent", ctor);
    JS_FreeValue(ctx, global);
}

void idb_version_change_event_init(JSContext *ctx)
{
    JSClassDef d = { "IDBVersionChangeEvent" };

    DCHECK(!g_ready, "idb_version_change_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "idbVersionChangeEventSlots", false);
    CHECK(!JS_IsException(g_key), "the IDBVersionChangeEvent slot key allocation failed");
    g_vce_rt = JS_GetRuntime(ctx);
    JS_NewClassID(g_vce_rt, &g_vce_class);
    CHECK(JS_NewClass(g_vce_rt, g_vce_class, &d) == 0,
          "IDBVersionChangeEvent: the per-realm prototype slot could not be declared");
    g_ctor_stepid = idl_method_id_dict(ctx, VCE_CTOR_ARGS, 2, VCE_INIT,
                                       (int)(sizeof VCE_INIT / sizeof VCE_INIT[0]), js_vce_ctor, 0);
    idl_optional_from(1);          /* `optional IDBVersionChangeEventInit eventInitDict = {}` */
    g_ready = 1;
    agent_state_flag("idb_version_change_event", &g_ready, "the declaration latch");
    agent_state_ptr("idb_version_change_event", &g_vce_rt, "the runtime §4.2's slot key was minted in");
    agent_state_value("idb_version_change_event", &g_key, "§4.2's internal-slot key");
    realm_declare_intrinsic(idb_version_change_event_install_realm);
}

void idb_version_change_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — see idb_transaction_free. */
    DCHECK(g_ready, "§4.2's interface was released in an agent that never declared it");
    DCHECK(rt == g_vce_rt, "idb_version_change_event_free was given a runtime that is not the one it declared "
                           "into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_vce_rt = NULL;
    g_ready = 0;
}
