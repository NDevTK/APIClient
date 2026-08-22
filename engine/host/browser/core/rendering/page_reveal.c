/* REVEAL A DOCUMENT — HTML §7.4.6.3, and `PageRevealEvent`. See page_reveal.h. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/rendering/page_reveal.h"

static int      g_slot = -1;          /* the per-realm "has been revealed" record (a baseline object) */
static JSAtom   g_atom_revealed = JS_ATOM_NULL;
static JSClassID g_class;             /* PageRevealEvent.prototype, in quickjs's own per-context slot */
static int      g_id_ctor = -1;
static JSValue  g_key = JS_UNDEFINED; /* the private Symbol the event's one slot lives under */
static int      g_ready;

/* ---- the interface ---------------------------------------------------------------------------------------
   `[Exposed=Window] interface PageRevealEvent : Event { constructor(DOMString type,
      optional PageRevealEventInit eventInitDict = {}); readonly attribute ViewTransition? viewTransition; }` */

static JSValue pr_slot(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots, v;

    if (!JS_IsObject(ev)) return JS_NULL;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_NULL;
    /* AN OWN SLOT, never a property LOOKUP: a lookup walks the prototype chain into the solver's absent-state
       seam and would mint a concolic for a name nobody defined. */
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_NULL; }
    v = JS_GetPropertyStr(ctx, slots, "viewTransition");
    JS_FreeValue(ctx, slots);
    return v;
}

static JSValue js_pr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return pr_slot(ctx, this_val);
}

/* Mint one over an Event this component then re-points at PageRevealEvent.prototype — the derived interface is
   a prototype chain, not a second kind of event object. */
static JSValue pr_new(JSContext *ctx, JSValue ev, JSValueConst transition)
{
    JSValue slots;

    if (JS_IsException(ev)) return ev;
    {
        JSValue proto = JS_GetClassProto(ctx, g_class);
        DCHECK(!JS_IsNull(proto),
               "PageRevealEvent.prototype was asked for in a realm that never ran its install");
        JS_SetPrototype(ctx, ev, proto);
        JS_FreeValue(ctx, proto);
    }
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "PageRevealEvent: OOM allocating its slot");
    JS_SetPropertyStr(ctx, slots, "viewTransition", JS_DupValue(ctx, transition));
    {
        JSAtom k = JS_ValueToAtom(ctx, g_key);
        CHECK(k != JS_ATOM_NULL, "the PageRevealEvent slot key could not be reached");
        JS_DefinePropertyValue(ctx, ev, k, slots, 0);   /* an internal slot: not enumerable, not writable */
        JS_FreeAtom(ctx, k);
    }
    return ev;
}

static const IdlArgType PR_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember PR_INIT[] = {   /* PageRevealEventInit, in IDL declaration order */
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "viewTransition", IDL_ANY },
};

static JSValue js_pr_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue transition, ev;
    const char *type;
    bool bubbles, cancelable;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor PageRevealEvent requires 'new'");
    DCHECK(argc >= 1, "PageRevealEvent's constructor reached its body with no type — §3.6 step 5's TypeError "
                      "is the declaration's");
    bubbles = idl_dict_bool(ctx, init, "bubbles");
    cancelable = idl_dict_bool(ctx, init, "cancelable");
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type) return JS_EXCEPTION;
    ev = event_new_untrusted(ctx, type, bubbles, cancelable);   /* §2.2: what the PAGE constructs is untrusted */
    JS_FreeCString(ctx, type);
    transition = idl_dict_get(ctx, init, "viewTransition");
    if (JS_IsUndefined(transition)) { JS_FreeValue(ctx, transition); transition = JS_NULL; }
    ev = pr_new(ctx, ev, transition);
    JS_FreeValue(ctx, transition);
    return ev;
}

/* ---- §7.4.6.3 -------------------------------------------------------------------------------------------- */

/* This document's record, OWNED. */
static JSValue pr_record(JSContext *ctx)
{
    JSValue rec;

    DCHECK(g_ready, "§7.4.6.3's record was reached before the component was declared");
    rec = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(rec),
           "a realm answered §7.4.6.3's `has been revealed` with no record — every Document has one, so this "
           "realm never ran page_reveal_install_proto and its first rendering opportunity would reveal it "
           "again on every frame");
    return rec;
}

bool page_reveal_pending(JSContext *ctx)
{
    JSValue rec = pr_record(ctx), v = JS_GetProperty(ctx, rec, g_atom_revealed);
    bool revealed = JS_ToBool(ctx, v);

    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return !revealed;
}

JSValue page_reveal_begin(JSContext *ctx)
{
    JSValue rec, ev;

    if (!page_reveal_pending(ctx))
        return JS_UNDEFINED;                       /* step 1 */
    rec = pr_record(ctx);
    JS_SetProperty(ctx, rec, g_atom_revealed, JS_TRUE);   /* step 2 */
    JS_FreeValue(ctx, rec);
    /* STEP 3: doc's ACTIVE VIEW TRANSITION. A document has one only if something started one, and every way to
       start one — `document.startViewTransition`, a cross-document transition's @view-transition rule — is
       part of CSS View Transitions, which this engine does not have. So the value is null, COMPUTED from the
       absence rather than assumed: the assert below fires the moment the interface exists, at the step that
       must then read a real transition. */
    {
        JSValue g = JS_GetGlobalObject(ctx), vt = JS_GetPropertyStr(ctx, g, "ViewTransition");
        bool present = !JS_IsUndefined(vt);

        JS_FreeValue(ctx, vt);
        JS_FreeValue(ctx, g);
        DCHECK(!present,
               "§7.4.6.3 step 3 reads doc's ACTIVE VIEW TRANSITION and this build now has ViewTransition — the "
               "reveal is still handing PageRevealEvent a null, and step 5 (activate the view transition) is "
               "not written at all. Build CSS View Transitions' document state here and in update-the-"
               "rendering step 18");
    }
    /* STEP 4: fire an event named `pagereveal` at doc's relevant global object, using PageRevealEvent. It does
       not bubble and is not cancelable — nothing in the algorithm reads a canceled flag. */
    ev = pr_new(ctx, event_new(ctx, "pagereveal", /*bubbles*/ false, /*cancelable*/ false), JS_NULL);
    DCHECK(!JS_IsException(ev), "the pagereveal event could not be minted");
    return ev;
}

void page_reveal_init(JSContext *ctx)
{
    DCHECK(!g_ready, "page_reveal_init ran twice — one declaration per agent");
    g_atom_revealed = JS_NewAtom(ctx, "hasBeenRevealed");
    CHECK(g_atom_revealed != JS_ATOM_NULL, "§7.4.6.3's own key could not be interned");
    g_key = JS_NewSymbol(ctx, "pageRevealSlots", false);
    CHECK(!JS_IsException(g_key), "the PageRevealEvent slot key allocation failed");
    {
        JSClassDef d = { "PageRevealEvent" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_class);
        JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    }
    g_slot = realm_value_declare(ctx, "§7.4.6.3 has been revealed");
    g_id_ctor = idl_method_id_dict(ctx, PR_CTOR_ARGS, 2, PR_INIT,
                                   (int)(sizeof(PR_INIT) / sizeof(PR_INIT[0])), js_pr_ctor, 0);
    idl_optional_from(1);   /* `optional PageRevealEventInit eventInitDict = {}` */
    g_ready = 1;
    realm_declare_intrinsic(page_reveal_install_proto);
}

void page_reveal_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base, rec;

    DCHECK(g_ready, "a realm asked for PageRevealEvent.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "page_reveal_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PageRevealEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PageRevealEvent");
    idl_install_accessor(ctx, proto, "viewTransition", js_pr_get, 0, -1);
    JS_SetClassProto(ctx, g_class, proto);

    /* THE DOCUMENT'S OWN RECORD, built with the realm so it belongs to the pre-boot BASELINE — the same reason
       §8.9's map is built here. Created inside a flow instead it would be that flow's private object and every
       sibling would read a `has been revealed` nobody wrote. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "§7.4.6.3's record could not be allocated");
    JS_SetProperty(ctx, rec, g_atom_revealed, JS_FALSE);
    realm_value_set(ctx, g_slot, rec);
}

void page_reveal_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "PageRevealEvent was installed before it was declared");
    ctor = idl_step_constructor(ctx, "PageRevealEvent", 1, g_id_ctor);
    CHECK(!JS_IsException(ctor), "the PageRevealEvent interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_class);
        DCHECK(!JS_IsNull(proto), "PageRevealEvent was installed in a realm with no prototype for it");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "PageRevealEvent", ctor);
}

void page_reveal_free(JSContext *ctx)
{
    if (!g_ready) return;
    g_ready = 0;
    /* The prototypes and the records are the REALMS' — each goes with its context. */
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_revealed);
    g_atom_revealed = JS_ATOM_NULL;
    g_slot = g_id_ctor = -1;
}
