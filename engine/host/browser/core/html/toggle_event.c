/* THE ToggleEvent INTERFACE — HTML §6.5.1 The ToggleEvent interface.
 *
 *     [Exposed=Window]
 *     interface ToggleEvent : Event {
 *       constructor(DOMString type, optional ToggleEventInit eventInitDict = {});
 *       readonly attribute DOMString oldState;
 *       readonly attribute DOMString newState;
 *       readonly attribute Element? source;
 *     };
 *     dictionary ToggleEventInit : EventInit {
 *       DOMString oldState = "";
 *       DOMString newState = "";
 *       Element? source = null;
 *     };
 *
 * WHY IT EXISTS RATHER THAN A PLAIN Event. §4.11.4's CLOSE THE DIALOG fires `beforetoggle` and then queues a
 * `toggle`, and the two attributes are the whole of what a handler reads: a page that watches one dialog for
 * both directions branches on `e.newState === "closed"`, and firing a plain Event there does not lose a
 * property quietly — it makes that comparison `undefined === "closed"`, so the handler takes the wrong arm and
 * every line after it runs in the wrong world. `source` is the element that caused the change, which for a
 * `method=dialog` submission is the submitter button.
 *
 * A REAL SUBCLASS, like SubmitEvent: its prototype chains to this realm's `Event.prototype`, so
 * `e instanceof Event` holds. The base half is event_new_derived's; the three own attributes hang off a private
 * Symbol, which makes them slots a page cannot forge and — because a slot written as a property write is
 * captured by the COW delta — state that time-travels with the flow that fired it.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype, for the
 * reason submit_event.c states: a hand-copied list of per-realm installs is a list that stops being
 * maintained. */
#include "check.h"
#include "quickjs.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/toggle_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_te_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static void toggle_event_install_realm(JSContext *ctx);

static JSValue te_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_te_class);

    DCHECK(!JS_IsNull(proto),
           "ToggleEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* §6.5.1: "The oldState and newState attributes must return the values they are initialized to." Both are the
   one slot record's, and `magic` IS the member, so there is one body rather than a copy per member.
   `source` IS NOT ONE OF THOSE TWO, and this comment used to say it was — it claimed the sentence above of all
   three, in the SubmitEvent singular ("the value it was initialized to") that §6.5.1 does not write. §6.5.1's
   own sentence for it is "The source getter steps are to return the result of retargeting source against
   this's currentTarget", which is an ALGORITHM and not a slot read — so the arm below runs it, through the one
   component §2.9's dispatch already retargets `relatedTarget` and every touch target with. (The algorithm is
   DOM §4.8 Interface ShadowRoot's "To retarget an object A against an object B"; §4.8 is where that standard
   defines it, beside the shadow-root concepts its three disjuncts are about.) */
enum { TE_OLD_STATE = 0, TE_NEW_STATE, TE_SOURCE };
static const char *const TE_SLOT_NAME[] = { "oldState", "newState", "source" };

static JSValue js_te_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a ToggleEvent attribute was read before toggle_event_init ran");
    DCHECK(magic >= TE_OLD_STATE && magic <= TE_SOURCE,
           "a ToggleEvent accessor was installed with a magic this interface has no member for");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a ToggleEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a ToggleEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, TE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    if (magic == TE_SOURCE) {
        /* §6.5.1's getter steps, whole. B is `this`'s currentTarget, which core/events/event.c initialises to
           null at every construction and unsets at end-of-dispatch, so it is an object or null at every read
           and never a miss — which is what §4.8's own second input requires. A is one of the three shapes the
           declared `Element?` admits: the IDL null, an Element, or an unknown the §3.2.17 member loop crossed;
           a value that is not a node has no root, so §4.8's first disjunct returns it unchanged and the
           unknown is not consumed by this read. */
        JSValue cur = event_current_target(ctx, this_val);
        JSValue out = event_target_retarget(ctx, v, cur);

        JS_FreeValue(ctx, cur);
        JS_FreeValue(ctx, v);
        return out;
    }
    return v;
}

/* The three own slots, placed on an event whose Event half is already built. `old_state` and `new_state` are
   VALUES rather than C strings because the constructor's arrive already converted by the IDL declaration and
   re-stringifying one would run the page's toString a second time. Returns -1 with the throw live. */
static int te_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst old_state, JSValueConst new_state,
                         JSValueConst source)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "oldState", JS_DupValue(ctx, old_state));
    JS_SetPropertyStr(ctx, slots, "newState", JS_DupValue(ctx, new_state));
    JS_SetPropertyStr(ctx, slots, "source", JS_DupValue(ctx, source));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue toggle_event_new(JSContext *ctx, const char *type, const char *old_state, const char *new_state,
                         JSValueConst source, bool bubbles, bool cancelable)
{
    JSValue tv, ov, nv, ev;

    DCHECK(g_ready, "a ToggleEvent was minted before toggle_event_init declared the interface");
    DCHECK(JS_IsNull(source) || element_is(source),
           "a ToggleEvent was minted with a `source` that is neither null nor an Element — the attribute is "
           "typed `Element?` and every algorithm that fires one produces exactly those two");
    tv = JS_NewString(ctx, type);
    ov = JS_NewString(ctx, old_state);
    nv = JS_NewString(ctx, new_state);
    if (JS_IsException(tv) || JS_IsException(ov) || JS_IsException(nv)) {
        JS_FreeValue(ctx, tv); JS_FreeValue(ctx, ov); JS_FreeValue(ctx, nv);
        return JS_EXCEPTION;
    }
    /* TRUSTED — the user agent fired it. `composed` is false: no algorithm in HTML fires this one composed. */
    ev = event_new_derived(ctx, te_proto(ctx), tv, bubbles, cancelable, /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (!JS_IsException(ev) && te_init_slots(ctx, ev, ov, nv, source) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, nv);
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * ToggleEventInit INHERITS EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first
 * and each level lexicographically among itself — which is the order this list is in, and the order a page pins
 * by throwing from one member's getter.
 *
 * `source` IS THE DECLARED TYPE `Element?` — IDL_INTERFACE_NULLABLE under §3.2.20 Nullable types over §3.2.15
 * Interface types, branded on the NODE class with `element_is` as the narrowing. It was IDL_ANY with the rule
 * written out in the body, on an argument that was TRUE and has been retired: IDL_INTERFACE brands against a
 * CLASS, every node wrapper in this engine is one class, so a declared interface could say only "a Node" and
 * would have crossed a Text node and a Document as an Element. A member now carries a NARROWING beside its
 * class (IdlDictMember::iface_narrow, and idl_iface_narrow for a declaration whose interface-typed members are
 * all one interface — which this dictionary's single one is), so the type is declarable exactly and performs
 * its own rule: null and undefined are the IDL null, an Element crosses as itself, anything else is a
 * TypeError at §3.2.15 step 2.
 *
 * AND WHAT MOVED IS NOT MERELY WHERE THE CHECK IS WRITTEN. The body's `is it an element` could not tell "this
 * is not an Element" from "this engine does not know what this is": unknown external input reaches a body
 * wearing the ordinary Object solver/concolic.c gives it, so a hand-rolled test answered NO for it and threw a
 * page-visible TypeError — a WRONG ANSWER handed to a script rather than a refusal, and the world in which the
 * value is the Element it will turn out to be was deleted with it. The declared type crosses an unknown as
 * ITSELF: `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES for this type, and §3.2.17's member loop rewrites a
 * CONCOLIC member whose rule is anything but FORKS to IDL_ANY before a type arm is asked — so the unknown
 * passes ahead of the nullable interface arm and reaches the slot, and the type refuses only values it has
 * ESTABLISHED are not Elements.
 *
 * THE ORDER DID NOT MOVE, AND SAYING SO IS PART OF THE CLAIM. §3.2.17's member loop converts in lexicographical
 * order and `source` sorts LAST among this dictionary's own members, so its TypeError still lands after
 * `newState` and `oldState` have been read and ToString'd — exactly where the body's threw. Where a declared
 * refusal DOES move earlier is a dictionary whose interface-typed member has members after it; this one has
 * none, so the gain here is the crossing above and nothing else. */
static const IdlArgType TE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember TE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "newState", IDL_DOMSTRING,           false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "oldState", IDL_DOMSTRING,           false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "source",   IDL_INTERFACE_NULLABLE,  false, NULL, 1, NULL, IDL_DEFAULT_NULL,   NULL },
};

static JSValue js_te_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue old_state, new_state, source, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor ToggleEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ToggleEvent constructor requires a type");
    /* `source` HAS NO CHECK HERE AND MUST NOT GAIN ONE. It is declared `Element?` with its own narrowing, so
       §3.2.20 ran at its own position in the conversion — step 3 for a null or an undefined, step 4 (§3.2.15's
       brand) for everything else — and what reaches this body is one of exactly THREE shapes: the IDL null, an
       Element, or an unknown the member loop crossed as itself. The third is why a shape assert would be WRONG
       and not merely redundant: an assert over the first two REFUSES the third, which is the world the crossing
       exists to keep open. The slot carries it, and the page's own branch on `e.source` is where it forks. */
    source = idl_dict_get(ctx, init, "source");
    old_state = idl_dict_get(ctx, init, "oldState");
    new_state = idl_dict_get(ctx, init, "newState");
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, te_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) && te_init_slots(ctx, ev, old_state, new_state, source) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, old_state);
    JS_FreeValue(ctx, new_state);
    JS_FreeValue(ctx, source);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_te_proto[] = {
    JS_CGETSET_MAGIC_DEF("oldState", js_te_get, NULL, TE_OLD_STATE),
    JS_CGETSET_MAGIC_DEF("newState", js_te_get, NULL, TE_NEW_STATE),
    JS_CGETSET_MAGIC_DEF("source", js_te_get, NULL, TE_SOURCE),
};

void toggle_event_init(JSContext *ctx)
{
    JSClassDef d = { "ToggleEvent" };

    DCHECK(!g_ready, "toggle_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "toggleEventSlots", false);
    CHECK(!JS_IsException(g_key), "the ToggleEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_te_class);
    JS_NewClass(JS_GetRuntime(ctx), g_te_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, TE_CTOR_ARGS, 2, TE_INIT,
                                       (int)(sizeof(TE_INIT) / sizeof(TE_INIT[0])), js_te_ctor, 0);
    /* `Element? source` — a NODE by class and an ELEMENT by narrowing, which is the pair a class id alone
       cannot state. The type is Element and NOT HTMLElement, so no namespace test belongs in it: an SVG
       element satisfies §6.5.1's `Element?` and `element_is` is the predicate that says so.
       IT IS STATED HERE, AT THE DECLARATION, AND NOT AT THE PER-REALM INSTALL. NavigateEventInit's four
       interface classes are written at install because that interface is declared from core/events/event.c's
       subclass list, whose core/platform.c row (`event`) runs BEFORE the `element` row that creates the node
       class — so reading it at declaration time would read a zero. This file is reached the other way round:
       toggle_event_init is called from html_dialog_declare, which core/dom/element.c's element_init reaches
       through html_element_init, LONG after its own node_init created that class. So the ordinary declaration
       form applies, and idl_iface_brand's own `iface != 0` refusal is what makes the day that stops being true
       an abort at this line rather than a silent zero read at a conversion. */
    idl_iface_brand(node_class_id());
    idl_iface_narrow(element_is);
    /* HTML §6.5.1 The ToggleEvent interface writes
       `constructor(DOMString type, optional ToggleEventInit eventInitDict = {})`, and the word `optional` was
       in no code: with position 1 required, Web IDL §3.6's step 5 arity check threw a TypeError for
       `new ToggleEvent("toggle")` — the ordinary one-argument construction. It ALSO made §3.7.1 Interface
       object's length 2 where the IDL computes 1, which is the half a `length` audit can see; the throw is
       the half it cannot. */
    idl_optional_from(1);
    g_ready = 1;
    realm_declare_intrinsic(toggle_event_install_realm);
}

static void toggle_event_install_realm(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for ToggleEvent before toggle_event_init declared it");
    prev = JS_GetClassProto(ctx, g_te_class);
    DCHECK(JS_IsNull(prev), "toggle_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "ToggleEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ToggleEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_te_proto, (int)(sizeof(js_te_proto) / sizeof(js_te_proto[0])));
    JS_SetClassProto(ctx, g_te_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `ToggleEvent` per realm, whose `prototype` is the
       prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "ToggleEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the ToggleEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "ToggleEvent", ctor);
    JS_FreeValue(ctx, global);
}

void toggle_event_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
