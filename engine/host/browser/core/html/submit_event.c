/* THE SubmitEvent INTERFACE — HTML §4.10.22.10.
 *
 *     [Exposed=Window]
 *     interface SubmitEvent : Event {
 *       constructor(DOMString type, optional SubmitEventInit eventInitDict = {});
 *       readonly attribute HTMLElement? submitter;
 *     };
 *     dictionary SubmitEventInit : EventInit { HTMLElement? submitter = null; };
 *
 * WHY IT IS NOT DECORATION. §4.10.22.3 step 5.6 fires the `submit` event USING THIS INTERFACE, and `submitter`
 * is the only thing that tells a handler WHICH button submitted the form — which is what decides the request:
 * a submit button carries `formaction`, `formmethod`, `formenctype` and `formtarget`, so one form posts to two
 * endpoints and the handler reads which one it is off this attribute. Firing a plain Event there does not
 * merely lose a property, it makes `e.submitter.name` a TypeError in every bundle that routes on it — the
 * handler throws, and the code after it (the fetch this engine exists to find) never runs.
 *
 * A REAL SUBCLASS, like FormDataEvent: its prototype chains to the realm's `Event.prototype`, so
 * `e instanceof Event` holds and `initEvent` works on one. The base half is event_new_derived's; the one own
 * attribute hangs off a private Symbol, which makes it a slot a page cannot forge and — because a slot written
 * as a property write is captured by the COW delta — state that time-travels with the flow that fired it.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype rather
 * than installed from a host's hand-written list of globals. §3.7 gives each realm its own interface OBJECT for
 * the same reason it gives each its own prototype, and realm.h exists precisely because a list of per-realm
 * installs that is hand-copied is a list that stops being maintained: a component missing from one copy is
 * silently absent in that realm with nothing to say so. */
#include "check.h"
#include "quickjs.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/html/html_element.h"
#include "core/html/submit_event.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slot hangs off */
static JSClassID g_se_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static void submit_event_install_realm(JSContext *ctx);

static JSValue se_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_se_class);

    DCHECK(!JS_IsNull(proto),
           "SubmitEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* §4.10.22.10: "The submitter attribute must return the value it was initialized to." */
static JSValue js_se_get_submitter(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    (void)magic;
    DCHECK(g_ready, "a SubmitEvent attribute was read before submit_event_init ran");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a SubmitEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a SubmitEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, "submitter");
    JS_FreeValue(ctx, slots);
    return v;
}

/* The one own slot, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int se_init_slot(JSContext *ctx, JSValueConst ev, JSValueConst submitter)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "submitter", JS_DupValue(ctx, submitter));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue submit_event_new(JSContext *ctx, JSValueConst submitter)
{
    JSValue tv = JS_NewString(ctx, "submit");
    /* §4.10.22.3 step 5.6 names three initialisers and no others: `submitter`, `bubbles` true and `cancelable`
       true. It is NOT composed, and it IS trusted — the user agent fired it. */
    JSValue ev = event_new_derived(ctx, se_proto(ctx), tv, /*bubbles*/ true, /*cancelable*/ true,
                                   /*composed*/ false, /*trusted*/ true);

    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    DCHECK(JS_IsNull(submitter) || html_element_is(submitter),
           "§4.10.22.3 step 5.5's submitterButton was neither null nor an HTMLElement — the attribute is typed "
           "`HTMLElement?` and step 5.5 produces exactly those two");
    if (se_init_slot(ctx, ev, submitter) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional SubmitEventInit eventInitDict = {})`. SubmitEventInit INHERITS
 * EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
 * lexicographically among itself — which is the order this list is in, and the order a page pins by throwing
 * from one member's getter.
 *
 * `submitter` IS THE DECLARED TYPE `HTMLElement?` — IDL_INTERFACE_NULLABLE under §3.2.20 Nullable types over
 * §3.2.15 Interface types, branded on the NODE class with `html_element_is` as the narrowing. It was IDL_ANY
 * with the rule written out in the body, and the placement was called the declaration surface's gap rather
 * than this member's choice, on two arguments of which ONE was never true and the other has been retired.
 * The one that was never true: "IDL_INTERFACE refuses null, which the IDL's `= null` default makes the
 * ordinary value" — that is what the NULLABLE row is for, and it existed then; §3.2.20 makes null and
 * undefined the IDL null ahead of any brand. The one that was true and is retired: IDL_INTERFACE brands
 * against a CLASS and every node wrapper in this engine is one class, so a declared interface could say only
 * "a Node" and would have crossed an SVG element as an HTMLElement. A member now carries a NARROWING beside
 * its class (IdlDictMember::iface_narrow, and idl_iface_narrow for a declaration whose interface-typed members
 * are all one interface — which this dictionary's single one is), and the namespace is the whole of that
 * narrowing: §4's element-interface table maps every HTML-namespace element to an interface INHERITING
 * HTMLElement, HTMLUnknownElement included, which is why `html_element_is` is the right answer and not a
 * stricter one.
 *
 * AND WHAT MOVED IS NOT MERELY WHERE THE CHECK IS WRITTEN. The body's `is it an HTML element` could not tell
 * "this is not an HTMLElement" from "this engine does not know what this is": unknown external input reaches a
 * body wearing the ordinary Object solver/concolic.c gives it, so a hand-rolled test answered NO for it and
 * threw a page-visible TypeError — a WRONG ANSWER handed to a script rather than a refusal, and the world in
 * which the value is the button it will turn out to be was deleted with it. That world is the one this engine
 * is for: `submitter` is what decides the request, since a submit button carries `formaction`, `formmethod`,
 * `formenctype` and `formtarget`. The declared type crosses an unknown as ITSELF: `idl_concolic_rule` answers
 * IDL_CONCOLIC_CROSSES for it, and §3.2.17's member loop rewrites a CONCOLIC member whose rule is anything but
 * FORKS to IDL_ANY before a type arm is asked — so the unknown passes ahead of the nullable interface arm and
 * reaches the slot, and the type refuses only values it has ESTABLISHED are not HTMLElements.
 *
 * `= null` IS DECLARED, AND IT WAS NOT. The IDL at the top of this file writes `HTMLElement? submitter = null`
 * and this row carried IDL_DEFAULT_NONE, which §3.2.17 leaves ABSENT — the body's own
 * `JS_IsUndefined(sub) → JS_NULL` was standing in for the declaration's default, so deleting the body without
 * the default would have made `new SubmitEvent("submit").submitter` answer `undefined` where every browser
 * answers `null`. The two go together or the deletion is a regression.
 *
 * THE ORDER DID NOT MOVE, AND SAYING SO IS PART OF THE CLAIM. §3.2.17's member loop converts in lexicographical
 * order and `submitter` is this dictionary's ONLY own member, so its TypeError lands where the body's threw.
 * Where a declared refusal DOES move earlier is a dictionary with members after the interface-typed one; this
 * one has none, so the gain here is the crossing above and the default below it. */
static const IdlArgType SE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember SE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "submitter", IDL_INTERFACE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL, NULL },
};

static JSValue js_se_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue sub, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor SubmitEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "SubmitEvent constructor requires a type");
    /* `submitter` HAS NO CHECK HERE AND MUST NOT GAIN ONE. It is declared `HTMLElement?` with its own
       narrowing, so §3.2.20 ran at its own position in the conversion — step 3 for a null or an undefined,
       step 4 (§3.2.15's brand) for everything else — and what reaches this body is one of exactly THREE
       shapes: the IDL null, an HTMLElement, or an unknown the member loop crossed as itself. The third is why
       a shape assert would be WRONG and not merely redundant: an assert over the first two REFUSES the third,
       which is the world the crossing exists to keep open. The slot carries it, and the page's own branch on
       `e.submitter` is where it forks. */
    sub = idl_dict_get(ctx, init, "submitter");
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, se_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev)) {
        JS_FreeValue(ctx, sub);
        return ev;
    }
    if (se_init_slot(ctx, ev, sub) < 0) {
        JS_FreeValue(ctx, sub);
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, sub);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_se_proto[] = {
    JS_CGETSET_MAGIC_DEF("submitter", js_se_get_submitter, NULL, 0),
};

void submit_event_init(JSContext *ctx)
{
    JSClassDef d = { "SubmitEvent" };

    DCHECK(!g_ready, "submit_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "submitEventSlots", false);
    CHECK(!JS_IsException(g_key), "the SubmitEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_se_class);
    JS_NewClass(JS_GetRuntime(ctx), g_se_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, SE_CTOR_ARGS, 2, SE_INIT,
                                       (int)(sizeof(SE_INIT) / sizeof(SE_INIT[0])), js_se_ctor, 0);
    /* `HTMLElement? submitter` — a NODE by class and an HTML-namespace ELEMENT by narrowing, which is the pair
       a class id alone cannot state, and the same pairing element_internals.c declares for §4.13.7.3's
       `optional HTMLElement anchor`.
       IT IS STATED HERE, AT THE DECLARATION, AND NOT AT THE PER-REALM INSTALL. NavigateEventInit's four
       interface classes are written at install because that interface is declared from core/events/event.c's
       subclass list, whose core/platform.c row (`event`) runs BEFORE the `element` row that creates the node
       class — so reading it at declaration time would read a zero. This file is reached the other way round:
       submit_event_init is called from html_form_declare, which core/dom/element.c's element_init reaches
       through html_element_init, LONG after its own node_init created that class. So the ordinary declaration
       form applies, and idl_iface_brand's own `iface != 0` refusal is what makes the day that stops being true
       an abort at this line rather than a silent zero read at a conversion. */
    idl_iface_brand(node_class_id());
    idl_iface_narrow(html_element_is);
    /* HTML §4.10.22.10 The SubmitEvent interface writes
       `constructor(DOMString type, optional SubmitEventInit eventInitDict = {})`, and the word `optional` was
       in no code: with position 1 required, Web IDL §3.6's step 5 arity check threw a TypeError for
       `new SubmitEvent("submit")` — the ordinary one-argument construction, which is what a page writes when
       it re-dispatches a form submission. It ALSO made §3.7.1 Interface object's length 2 where the IDL
       computes 1, which is the half a `length` audit can see; the throw is the half it cannot. */
    idl_optional_from(1);
    g_ready = 1;
    realm_declare_intrinsic(submit_event_install_realm);
}

static void submit_event_install_realm(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for SubmitEvent before submit_event_init declared it");
    prev = JS_GetClassProto(ctx, g_se_class);
    DCHECK(JS_IsNull(prev), "submit_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "SubmitEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "SubmitEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_se_proto, (int)(sizeof(js_se_proto) / sizeof(js_se_proto[0])));
    JS_SetClassProto(ctx, g_se_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `SubmitEvent` per realm, whose `prototype` is the
       prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "SubmitEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the SubmitEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "SubmitEvent", ctor);
    JS_FreeValue(ctx, global);
}

void submit_event_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
