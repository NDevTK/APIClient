/* REVEAL A DOCUMENT — HTML §7.4.6.3, and `PageRevealEvent`. See page_reveal.h. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/rendering/page_reveal.h"

static int      g_slot = -1;          /* the per-realm "has been revealed" record (a baseline object) */
static JSAtom   g_atom_revealed = JS_ATOM_NULL;
static JSClassID g_class;             /* §7.2.7.5's prototype slot, in quickjs's own per-context table */
static int      g_id_ctor = -1;
static JSValue  g_key = JS_UNDEFINED; /* the private Symbol the event's one slot lives under */
/* THE RUNTIME THIS COMPONENT WAS DECLARED IN, and the only slot that says "declared". It replaces a `g_ready`
   flag, and that is not a rename: a flag answers "did init run", this answers "did init run, AND in which
   runtime" — which is strictly more, and is exactly what page_reveal_free asserts it is undoing. Keeping both
   would be one fact with two spellings free to disagree, which is the shape core/agent_state.h opens by
   describing. */
static JSRuntime *g_rt;

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

    DCHECK(g_rt != NULL, "§7.4.6.3's record was reached before the component was declared, or after "
                         "page_reveal_free gave its slot back");
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
    /* THE LATCH, AND IT COMPILES OUT IN RELEASE — which is why the class id may not be carried past the
       release. There is no early return here, so under -DAPICLIENT_DEV=0 a second agent runs this whole body,
       and JS_NewClassID hands a NON-ZERO slot back UNCHANGED rather than allocating: the CHECK below would
       then ask JS_NewClass for a number the new runtime's own allocator never issued and is about to issue to
       whichever component asks next. */
    DCHECK(g_rt == NULL, "page_reveal_init ran twice — one declaration per agent");
    g_rt = JS_GetRuntime(ctx);
    g_atom_revealed = JS_NewAtom(ctx, "hasBeenRevealed");
    CHECK(g_atom_revealed != JS_ATOM_NULL, "§7.4.6.3's own key could not be interned");
    g_key = JS_NewSymbol(ctx, "pageRevealSlots", false);
    CHECK(!JS_IsException(g_key), "the PageRevealEvent slot key allocation failed");
    {
        JSClassDef d = { "PageRevealEvent" };
        JS_NewClassID(g_rt, &g_class);
        CHECK(JS_NewClass(g_rt, g_class, &d) == 0,
              "PageRevealEvent: the per-realm prototype slot could not be declared");
    }
    g_slot = realm_value_declare(ctx, "§7.4.6.3 has been revealed");
    g_id_ctor = idl_method_id_dict(ctx, PR_CTOR_ARGS, 2, PR_INIT,
                                   (int)(sizeof(PR_INIT) / sizeof(PR_INIT[0])), js_pr_ctor, 0);
    idl_optional_from(1);   /* `optional PageRevealEventInit eventInitDict = {}` */
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. It declared NOTHING while its
       row's release column was EMPTY, which is the pair of silences core/platform.c's list reads as agreement:
       a component that holds everything and gives none of it back produces character-for-character the report
       a component that holds nothing produces. It held five slots and gave four of them back; the one it kept
       is the CLASS ID. */
    agent_state_ptr("page_reveal", &g_rt,
                    "the runtime HTML §7.2.7.5 The PageRevealEvent interface's class, §7.4.6.3 Revealing the "
                    "document's realm-value slot and the constructor declaration were registered in");
    agent_state_class("page_reveal", &g_class,
                      "HTML §7.2.7.5 The PageRevealEvent interface's per-realm prototype slot");
    agent_state_value("page_reveal", &g_key,
                      "§7.2.7.5's internal-slot key, the Symbol `viewTransition` lives under");
    agent_state_atom("page_reveal", &g_atom_revealed,
                     "HTML §7.4.6.3 Revealing the document's `has been revealed`, interned as the key of the "
                     "per-realm record");
    agent_state_id("page_reveal", &g_slot,
                   "the per-realm slot HTML §7.4.6.3 Revealing the document's `has been revealed` record "
                   "lives in");
    agent_state_id("page_reveal", &g_id_ctor,
                   "HTML §7.2.7.5 The PageRevealEvent interface's constructor declaration");
    realm_declare_intrinsic(page_reveal_install_proto);
}

void page_reveal_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base, rec;

    DCHECK(g_rt != NULL, "a realm asked for PageRevealEvent.prototype before the interface was declared");
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
       §8.12 Animation frames's map is built here. Created inside a flow instead it would be that flow's private object and every
       sibling would read a `has been revealed` nobody wrote. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "§7.4.6.3's record could not be allocated");
    JS_SetProperty(ctx, rec, g_atom_revealed, JS_FALSE);
    realm_value_set(ctx, g_slot, rec);
}

void page_reveal_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_rt != NULL, "PageRevealEvent was installed before it was declared");
    ctor = idl_step_constructor(ctx, "PageRevealEvent", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the PageRevealEvent interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_class);
        DCHECK(!JS_IsNull(proto), "PageRevealEvent was installed in a realm with no prototype for it");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "PageRevealEvent", ctor);
}

/* THE AGENT'S HALF, UNDONE — a row on core/platform.h's release column, and it takes the RUNTIME because that
 * is what an agent is. It took a JSContext until this diff and used it for nothing but JS_FreeValue and
 * JS_FreeAtom, which are JS_FreeValueRT(ctx->rt, v) and JS_FreeAtomRT(ctx->rt, a); that signature is the whole
 * of what kept this component off the column and made it a hand-written line in three hosts instead.
 *
 * NOTHING READS THIS COMPONENT AFTER IT RUNS, and that is a claim about a list rather than a hope. The only
 * file outside this one that names any symbol of it is core/rendering/rendering.c — `page_reveal_pending` in
 * its step-4 test and `page_reveal_begin` at step 6 — and `rendering` is the row AFTER this one, so on reverse
 * declaration order rendering_free has already run by the time this line is reached. No other release on the
 * column and no host teardown line names a static of this file.
 *
 * AND THERE IS NO COLLECTOR ENTRY: `JSClassDef d = { "PageRevealEvent" }` leaves finalizer and gc_mark null,
 * so core/agent_state.h's closing obligation — an entry running after the release column must reach its record
 * through JS_GetAnyOpaque rather than by looking up an id this line has given back — has nothing here to apply
 * to. Its twin in core/css/media_query_list.c did, and that is the leak this group was worth landing for. */
void page_reveal_free(JSRuntime *rt)
{
    /* NOT `if (!g_rt) return;`. This is a row on core/platform.c's list, whose declare pass is unconditional
       and which runs only where platform_agent_init ran, so a null runtime here is a host tearing down a
       browser it never built — and a silent return would make that indistinguishable from a release that
       worked. */
    DCHECK(g_rt != NULL,
           "§7.4.6.3's reveal was released in an agent that never declared it — page_reveal_init is a row on "
           "core/platform.c's declare column, so reaching here without it is a teardown of a browser that was "
           "never brought up");
    DCHECK(g_rt == rt,
           "§7.4.6.3's reveal was released against a RUNTIME other than the one it was declared in — its "
           "class, its realm-value slot and its constructor declaration are registrations in that runtime, and "
           "zeroing them against another leaves every one of them standing in the runtime that issued them");
    /* The prototypes and the records are the REALMS' — each goes with its context. */
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_revealed);
    g_atom_revealed = JS_ATOM_NULL;
    g_slot = g_id_ctor = -1;
    /* AND THE CLASS ID, which this release kept. core/agent_state.h settles it: a class is registered in a
       RUNTIME, so a carried id names a class in a runtime that is gone — and because JS_NewClassID returns a
       non-zero slot UNCHANGED rather than allocating, a second agent's page_reveal_init would hand JS_NewClass
       a number the new runtime's own allocator never issued and will issue to whichever component asks next.
       There is no brand site in this file to invert when it goes: g_class is read only by JS_GetClassProto,
       JS_SetClassProto and the mint, none of which any release reaches. */
    g_class = 0;
    g_rt = NULL;
}
