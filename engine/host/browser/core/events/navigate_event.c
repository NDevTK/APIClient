/* THE NavigateEvent INTERFACE — HTML §7.2.6.10.1. See navigate_event.h for what is deliberately absent.
 *
 *     [Exposed=Window]
 *     interface NavigateEvent : Event {
 *       constructor(DOMString type, NavigateEventInit eventInitDict);
 *       readonly attribute NavigationType navigationType;
 *       readonly attribute NavigationDestination destination;
 *       readonly attribute boolean canIntercept;
 *       readonly attribute boolean userInitiated;
 *       readonly attribute boolean hashChange;
 *       readonly attribute AbortSignal signal;
 *       readonly attribute FormData? formData;
 *       readonly attribute DOMString? downloadRequest;
 *       readonly attribute any info;
 *       readonly attribute boolean hasUAVisualTransition;
 *       readonly attribute Element? sourceElement;
 *       undefined intercept(optional NavigationInterceptOptions options = {});
 *       undefined scroll();
 *     };
 *
 * ELEVEN ATTRIBUTES AND ONE SENTENCE FOR ALL OF THEM: "the navigationType, destination, canIntercept,
 * userInitiated, hashChange, signal, formData, downloadRequest, info, hasUAVisualTransition, and sourceElement
 * attributes must return the values they are initialized to." So this file holds ONE slot record and one
 * magic-indexed getter, and every interesting decision is §7.2.6.10.4's — which is where they are made.
 *
 * THE CLASSIC HISTORY API STATE IS THE TWELFTH SLOT AND IS NOT AN IDL MEMBER. §7.2.6.10.1: "each NavigateEvent
 * has a classic history API state, a serialized state or null. It is only used in some cases where the event's
 * navigationType is 'push' or 'replace'". §7.2.6.10.4's commit reads it back to hand to the URL and history
 * update steps, which is the whole of how a `pushState`'s data survives an interception — so it is state of the
 * event and not of the algorithm that fired it, and it is BYTES because a serialized state crosses a park.
 *
 * ITS DICTIONARY DECLARES FOUR DIFFERENT INTERFACE TYPES, which is what made IdlDictMember::iface necessary:
 * `destination` is a NavigationDestination and `signal` an AbortSignal, both REQUIRED; `FormData? formData` and
 * `Element? sourceElement` are the same section under §3.2.20 Nullable types — T? step 3, "Otherwise, if V is
 * null or undefined, then return the IDL nullable type T? value null". All four are branded by the
 * DECLARATION, each against its own class and — for `sourceElement`, whose class is every node wrapper in this
 * engine — its own narrowing.
 *
 * THE TWO NULLABLE ONES WERE DECLARED `any` AND CHECKED IN THIS BODY, AND THAT COST A WRONG ANSWER TO THE PAGE.
 * A hand-rolled `else if (!form_data_is(v)) return JS_ThrowTypeError(...)` is §3.2.15's own step 2 written out
 * one stage too late, and it cannot tell a value that is not a FormData from one this engine DOES NOT KNOW:
 * §3.2.17 Dictionary types' member loop crosses unknown external input as ITSELF before any type arm is asked,
 * so `{formData: <unknown>}` arrived here wearing the Object solver/concolic.c gives it and was told its value
 * was invalid — a TypeError the page can SEE and act on, asserting something the engine had not established,
 * and deleting the world in which the value was a FormData all the same. Declared, that value crosses and
 * reaches the slot as itself, and every later branch on `e.formData` forks where the page branches. The
 * placement was wrong in a second, page-visible way even for a real wrong value: §3.2.17 converts member by
 * member in lexicographic order, so `formData`'s TypeError is owed BEFORE `hasUAVisualTransition`, `info` and
 * `navigationType` are read at all, and a body runs after every one of them. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/abort.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/navigate_event.h"
#include "core/frame/navigation_destination.h"
#include "core/html/form_data.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/structured_clone.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off — and the BRAND */
static JSClassID g_nav_ev_class;/* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

/* §7.2.6.3's `enum NavigationType { "push", "replace", "reload", "traverse" }` — the TYPE of the
   `navigationType` member, so the list is what the declaration carries and no body re-states it. */
static const char *const NAVIGATION_TYPE[] = { "push", "replace", "reload", "traverse", NULL };

/* The eleven IDL attributes, plus the one internal slot that is not one. `magic` IS the member. */
enum { NE_NAVIGATION_TYPE = 0, NE_DESTINATION, NE_CAN_INTERCEPT, NE_USER_INITIATED, NE_HASH_CHANGE, NE_SIGNAL,
       NE_FORM_DATA, NE_DOWNLOAD_REQUEST, NE_INFO, NE_HAS_UA_VISUAL_TRANSITION, NE_SOURCE_ELEMENT, NE_N };
static const char *const NE_NAME[] = {
    "navigationType", "destination", "canIntercept", "userInitiated", "hashChange", "signal",
    "formData", "downloadRequest", "info", "hasUAVisualTransition", "sourceElement"
};
#define NE_CLASSIC_STATE "classicHistoryAPIState"   /* the serialized bytes, or JS_NULL */

static JSValue ne_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_nav_ev_class);

    DCHECK(!JS_IsNull(proto), "NavigateEvent.prototype was asked for in a realm that never ran its per-realm "
                              "install");
    return proto;   /* OWNED */
}

/* The slot record of a NavigateEvent. OWNED. */
static JSValue ne_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue slots;

    CHECK(k != JS_ATOM_NULL, "NavigateEvent: the slot key could not be resolved to an atom");
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

static JSValue ne_field(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSValue slots = ne_slots(ctx, ev), v;

    DCHECK(JS_IsObject(slots), "a NavigateEvent carried no slot record — every instance is built through "
                               "ne_init_slots, which places one before it returns");
    v = JS_GetPropertyStr(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return v;
}

/* WEB IDL §3.7.6 Attributes' BRAND, asked of the OWN SLOT RECORD — never of a class id, which is a question
   no instance
   of this interface has ever answered yes to. The class an event interface declares exists for its per-realm
   PROTOTYPE SLOT and nothing wears it: every event in this engine is minted by core/events/event.c's
   event_make_proto through JS_NewObjectProto, so JS_GetClassID of a NavigateEvent is JS_CLASS_OBJECT, for the
   one the firing algorithm builds and for the one a page constructs alike. A brand written against
   g_nav_ev_class therefore answers NO universally, and the failure is silent everywhere it is an `if`
   (HTML §7.2.6.4 Initializing and updating the entry list step 14 skipped its intercept commit handler steps)
   and a TypeError everywhere it guards a getter. The brand that DOES distinguish this interface is the private
   Symbol ne_init_slots hangs its record off — the same test core/events/event.c's event_is makes of the base
   interface and core/html/submit_event.c makes of its own. */
bool navigate_event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots;
    bool is;

    DCHECK(g_ready, "a value was asked whether it is a NavigateEvent before the interface was declared — the "
                    "slot key the question is asked with is minted by navigate_event_init");
    /* NOT AN OBJECT IS NOT ONE, answered here rather than by the slot read: JS_GetOwnSlot THROWS on a
       non-object, and this predicate's callers are assertions and an `if`, neither of which would clear the
       pending TypeError it would leave behind. */
    if (!JS_IsObject(v))
        return false;
    slots = ne_slots(ctx, v);
    is = JS_IsObject(slots);
    JS_FreeValue(ctx, slots);
    return is;
}

JSValue navigate_event_signal(JSContext *ctx, JSValueConst ev)
{
    JSValue v;

    DCHECK(navigate_event_is(ctx, ev),
           "§7.2.6.10.4 asked for the abort controller's signal of something that is not a NavigateEvent");
    v = ne_field(ctx, ev, NE_NAME[NE_SIGNAL]);
    DCHECK(abort_signal_is(ctx, v),
           "a NavigateEvent's `signal` slot held something that is not an AbortSignal — both producers place "
           "one there, the firing algorithm from the event's own abort controller and the constructor from a "
           "`required AbortSignal signal` the declaration has already branded");
    return v;
}

/* ---- the eleven attributes -------------------------------------------------------------------------------- */

static JSValue js_ne_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DCHECK(g_ready, "a NavigateEvent attribute was read before its init ran");
    DCHECK(magic >= NE_NAVIGATION_TYPE && magic < NE_N,
           "a NavigateEvent accessor was installed with a magic this interface has no member for");
    /* WEB IDL §3.7.6 Attributes' BRAND, as the ONE predicate above and not a second copy of it: a getter
       pulled off the
       prototype and applied to something else is the TypeError a browser answers with rather than a read of a
       slot that is not there — and NavigateEvent.prototype itself fails it, because the record is an OWN slot
       of each instance and the prototype carries none. */
    if (!navigate_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "a NavigateEvent attribute was read on something that is not one");
    return ne_field(ctx, this_val, NE_NAME[magic]);
}

/* The twelve own slots, placed on an event whose Event half is already built. Every value is DUPPED: the
   destination and the signal are the very objects the algorithm holds, because a page compares
   `e.signal === e.signal` and passes the destination on.
   THE ARRAY IS ONE KIND OF VALUE AND THE CALLER FREES ALL OF IT. Both producers build a single JSValue array in
   which the borrowed arguments have already been dupped, so there is no per-index ownership rule for a later
   reader to get wrong — which is the shape §C-stack's "a struct copied field-by-field must dup EVERY owned
   field" is about, one level down. Returns -1 with the throw live. */
static int ne_init_slots(JSContext *ctx, JSValueConst ev, const JSValue *members, JSValueConst classic)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    int i;

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    for (i = 0; i < NE_N; i++)
        JS_SetPropertyStr(ctx, slots, NE_NAME[i], JS_DupValue(ctx, members[i]));
    JS_SetPropertyStr(ctx, slots, NE_CLASSIC_STATE, JS_DupValue(ctx, classic));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    /* THE PRODUCER AND THE BRAND ARE ASSERTED AGAINST EACH OTHER, at the ONE point both producers converge.
       The record this function just placed IS what navigate_event_is asks for, so the two coming apart is the
       state that must be impossible — and while it was not, nothing said so HERE: the first algorithm to ask
       the question aborted nine steps downstream at HTML §7.2.6.8 Ongoing navigation tracking's field write,
       naming a value the wrong type rather than the mint that gave it one. */
    DCHECK(navigate_event_is(ctx, ev),
           "a NavigateEvent left ne_init_slots without answering this interface's own BRAND — the slot record "
           "it just placed is that brand, so the mint and the predicate have come apart and every algorithm "
           "that asks navigate_event_is about an instance would answer no");
    return 0;
}

JSValue navigate_event_new_to_fire(JSContext *ctx, const char *navigation_type, JSValueConst destination,
                                   bool can_intercept, bool cancelable, bool user_initiated, bool hash_change,
                                   JSValueConst signal, JSValueConst source_element,
                                   const StructuredData *classic_state)
{
    JSValue members[NE_N];
    JSValue tv, ev, classic = JS_NULL;
    int i;

    DCHECK(g_ready, "a navigate event was fired before its interface was declared");
    DCHECK(navigation_type != NULL, "§7.2.6.10.4 initialized a NavigateEvent's `navigationType` with nothing — "
                                    "each of its three wrappers passes one of §7.2.6.3's four values");
    DCHECK(JS_GetClassID(destination) == navigation_destination_class(),
           "§7.2.6.10.4 initialized a NavigateEvent's `destination` with something that is not a "
           "NavigationDestination");
    DCHECK(abort_signal_is(ctx, signal),
           "§7.2.6.10.4 initialized a NavigateEvent's `signal` with something that is not an AbortSignal — it "
           "is the event's own abort controller's signal, minted a line earlier");
    DCHECK(JS_IsNull(source_element) || element_is(source_element),
           "§7.2.6.10.4 initialized a NavigateEvent's `sourceElement` with something that is neither an Element "
           "nor null — the attribute is typed `Element?` and the responsible element is a link, a form or a "
           "submit button");
    members[NE_NAVIGATION_TYPE] = JS_NewString(ctx, navigation_type);
    if (JS_IsException(members[NE_NAVIGATION_TYPE])) return members[NE_NAVIGATION_TYPE];
    if (classic_state) {
        classic = JS_NewArrayBufferCopy(ctx, classic_state->buf, classic_state->len);
        CHECK(!JS_IsException(classic), "NavigateEvent: the classic history API state could not be allocated");
    }
    members[NE_DESTINATION] = JS_DupValue(ctx, destination);
    members[NE_CAN_INTERCEPT] = JS_NewBool(ctx, can_intercept);
    members[NE_USER_INITIATED] = JS_NewBool(ctx, user_initiated);
    members[NE_HASH_CHANGE] = JS_NewBool(ctx, hash_change);
    members[NE_SIGNAL] = JS_DupValue(ctx, signal);
    /* §7.2.6.10.4's push/replace/reload and traverse wrappers both reach the inner algorithm with
       formDataEntryList null and downloadRequestFilename null, so both members initialize to null — a computed
       answer about those two call sites and not a placeholder. A FORM SUBMISSION and a DOWNLOAD REQUEST are the
       two navigations that fill them, and neither algorithm exists in this build. */
    members[NE_FORM_DATA] = JS_NULL;
    members[NE_DOWNLOAD_REQUEST] = JS_NULL;
    /* "If apiMethodTracker is not null, then initialize event's info to apiMethodTracker's info. Otherwise,
       initialize it to undefined." Every caller reaching this build passes no tracker — §7.2.6.7's methods are
       the only source of one — so the value is undefined, which is also what the IDL's `any info` with no
       default gives a page that constructs one. */
    members[NE_INFO] = JS_UNDEFINED;
    /* "Initialize event's hasUAVisualTransition to true if a VISUAL TRANSITION, to display a cached rendered
       state of the document's latest entry, was done by the user agent. Otherwise, initialize it to false."
       This user agent performs none — the same answer core/frame/session_history.c gives §7.4.6.2's popstate,
       and for the same reason. */
    members[NE_HAS_UA_VISUAL_TRANSITION] = JS_FALSE;
    members[NE_SOURCE_ELEMENT] = JS_DupValue(ctx, source_element);

    /* "Initialize event's type to 'navigate'". DOM §2.5 "Constructing events" with THIS interface's prototype; the
       event does not BUBBLE (§7.2.6.10 declares no bubbling and it is fired at the Navigation, which has no
       tree) and is not COMPOSED. TRUSTED, because the user agent fired it. */
    tv = JS_NewString(ctx, "navigate");
    if (JS_IsException(tv)) {
        ev = tv;
    } else {
        ev = event_new_derived(ctx, ne_proto(ctx), tv, /*bubbles*/ false, cancelable, /*composed*/ false,
                               /*trusted*/ true);
        JS_FreeValue(ctx, tv);
        if (!JS_IsException(ev) && ne_init_slots(ctx, ev, members, classic) < 0) {
            JS_FreeValue(ctx, ev);
            ev = JS_EXCEPTION;
        }
    }
    for (i = 0; i < NE_N; i++) JS_FreeValue(ctx, members[i]);
    JS_FreeValue(ctx, classic);
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, NavigateEventInit eventInitDict)` — and the dictionary is NOT optional, which is
 * what the arity of 2 states and what makes a one-argument construction a TypeError before this body runs. That
 * follows from `required NavigationDestination destination` and `required AbortSignal signal`: a dictionary with
 * a required member has no `= {}` to default to.
 *
 * The member list is in Web IDL's conversion order — the INHERITED EventInit members first, then this
 * dictionary's own lexicographically. That order is observable: a page that throws from one member's getter
 * pins which of them was read first. */
static const IdlArgType NE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static IdlDictMember NE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "canIntercept",          IDL_BOOLEAN,           false, NULL,            1 },
    { "destination",           IDL_INTERFACE,         true,  NULL,            1 },
    { "downloadRequest",       IDL_DOMSTRING_NULLABLE, false, NULL,           1, NULL, IDL_DEFAULT_NULL, NULL },
    { "formData",              IDL_INTERFACE_NULLABLE, false, NULL,           1, NULL, IDL_DEFAULT_NULL, NULL },
    { "hasUAVisualTransition", IDL_BOOLEAN,           false, NULL,            1 },
    { "hashChange",            IDL_BOOLEAN,           false, NULL,            1 },
    { "info",                  IDL_ANY,               false, NULL,            1 },
    { "navigationType",        IDL_ENUM,              false, NAVIGATION_TYPE, 1, NULL, IDL_DEFAULT_STRING,
                                                                                       "push" },
    { "signal",                IDL_INTERFACE,         true,  NULL,            1 },
    { "sourceElement",         IDL_INTERFACE_NULLABLE, false, NULL,           1, NULL, IDL_DEFAULT_NULL, NULL },
    { "userInitiated",         IDL_BOOLEAN,           false, NULL,            1 },
};

static JSValue js_ne_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue members[NE_N], ev;
    int i;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor NavigateEvent requires 'new'");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "NavigateEvent constructor requires a type and an eventInitDict");
    for (i = 0; i < NE_N; i++)
        members[i] = idl_dict_get(ctx, init, NE_NAME[i]);
    /* TWO REQUIRED MEMBERS, TWO REFUSALS, because a joint one names NEITHER. `destination` and `signal` are two
       members declared two different interfaces, so a reader standing at one abort has to be told which of them
       came in wrong — and the message that fired named the pair, the rule and no subject, which is an action
       with no object. THE ONLY SHAPE EITHER CAN REACH THIS BODY IN is a branded object or an unknown the
       §3.2.17 member loop crossed past the brand arm; everything else is §3.2.15 Interface types step 2's
       "Throw a TypeError." before the body is entered.
       BOTH REFUSALS ARE INTERMEDIATE AND THE FORK EACH IS OWED IS THE SAME ONE. Over an unknown, §3.2.15's two
       steps are both feasible — this engine has not established that the value implements the interface and has
       not established that it does not — so the worlds are the TypeError of step 2 and a construction whose
       slot holds a NavigationDestination (an AbortSignal). What blocks the second arm is that §7.2.6.10.1's
       attributes must return "the values they are initialized to" and every algorithm that reads them reads a
       PLATFORM OBJECT — navigate_event_signal hands the slot to core/dom/abort.c, §7.2.6.10.4's commit reads
       the destination's URL — so the arm needs an object this engine cannot mint out of an unknown, which is
       not a fork this file can ask for on its own. Until there is one, this abort is the engine declining to
       explore a world the unknown admits, and it says so rather than blaming a conversion. */
    IDL_DCHECK_MEMBER(JS_GetClassID(members[NE_DESTINATION]) == navigation_destination_class(),
                      members[NE_DESTINATION], "destination",
                      "`required NavigationDestination destination` by HTML §7.2.6.10.1 The NavigateEvent "
                      "interface — branded per member (IdlDictMember::iface) over IDL_INTERFACE");
    IDL_DCHECK_MEMBER(abort_signal_is(ctx, members[NE_SIGNAL]), members[NE_SIGNAL], "signal",
                      "`required AbortSignal signal` by HTML §7.2.6.10.1 The NavigateEvent interface — branded "
                      "per member (IdlDictMember::iface) over IDL_INTERFACE");
    /* `FormData? formData = null` AND `Element? sourceElement = null` HAVE NO CHECK HERE AND MUST NOT GAIN ONE.
       Both are declared IDL_INTERFACE_NULLABLE with their own class, so §3.2.20 Nullable types — T? ran at each
       member's own position in the conversion — its step 3 for a null or an undefined, its step 4 (the inner
       type, which is §3.2.15 Interface types) for everything else — and what reaches this body is therefore one
       of exactly THREE shapes: the IDL null, an instance of the member's own interface, or an unknown the
       member loop crossed as itself. The third is why a shape assert here would be WRONG and not merely
       redundant: an assert over the first two REFUSES the third, which is the world the crossing exists to keep
       open. The slot carries it, and the page's own branch on `e.formData` is where it forks. */
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is untrusted,
       and
       §7.2.6.10.1's shared checks read exactly that off `isTrusted` when `intercept()` lands.
       Its CLASSIC HISTORY API STATE is null: NavigateEventInit declares no member for it, because it is an
       internal slot §7.2.6.10.4 sets and not something a page may hand over. */
    ev = event_new_derived(ctx, ne_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) && ne_init_slots(ctx, ev, members, JS_NULL) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    for (i = 0; i < NE_N; i++) JS_FreeValue(ctx, members[i]);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

void navigate_event_init(JSContext *ctx)
{
    JSClassDef d = { "NavigateEvent" };

    DCHECK(!g_ready, "navigate_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "navigateEventSlots", false);
    CHECK(!JS_IsException(g_key), "the NavigateEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nav_ev_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_nav_ev_class, &d) == 0,
          "NavigateEvent: the per-realm prototype slot could not be declared");
    g_ctor_stepid = idl_method_id_dict(ctx, NE_CTOR_ARGS, 2, NE_INIT,
                                       (int)(sizeof(NE_INIT) / sizeof(NE_INIT[0])), js_ne_ctor, 0);
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "HTML §7.2.6.10.1 The NavigateEvent interface's declaration latch");
    agent_state_class("event", &g_nav_ev_class,
                      "HTML §7.2.6.10.1 The NavigateEvent interface's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol HTML §7.2.6.10.1 The NavigateEvent interface's slot record and BRAND hang "
                      "off");
    agent_state_id("event", &g_ctor_stepid,
                   "HTML §7.2.6.10.1 The NavigateEvent interface's `constructor(DOMString type, NavigateEventInit "
                   "eventInitDict)`");
    realm_declare_intrinsic(navigate_event_install_protos);
}

void navigate_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;
    int i;

    DCHECK(g_ready, "a realm asked for NavigateEvent before its init declared it");
    /* ALL FOUR INTERFACE-TYPED MEMBERS CARRY THEIR OWN CLASSES, and they are read HERE rather than at the
       declaration because a class id is a RUNTIME registration made in core/platform.c's row order: this
       interface is declared from core/events/event.c's subclass list, which runs BEFORE the `abort` and
       `element` rows. A class id is agent-scoped — one per JSRuntime, not one per realm — so reading it at the
       first realm's install reads the same id every later realm would, and writing it again on each install
       writes the same value. That is why this is not the module static §per-realm-fact forbids: the fact being
       answered is the AGENT's, and the assertions inside each class accessor fire if either component has not
       declared yet. The declaration pass runs to its end before any realm's intrinsics begin, so every one of
       these four is registered by the time this line runs.
       AND `sourceElement` CARRIES A NARROWING BESIDE ITS CLASS, because a class cannot say what its type says.
       Every node wrapper in this engine is ONE class, so `node_class_id()` answers "a Node" for a Text node and
       for a Document as readily as for an Element, while HTML §7.2.6.10.1 The NavigateEvent interface writes
       `Element? sourceElement = null`. The other three classes name their interfaces exactly and state NULL,
       which is a positive statement and not an omission. */
    NE_INIT[4].iface = navigation_destination_class();     /* `destination` */
    NE_INIT[6].iface = form_data_class_id();               /* `formData` */
    NE_INIT[11].iface = abort_signal_class();              /* `signal` */
    NE_INIT[12].iface = node_class_id();                   /* `sourceElement` */
    NE_INIT[12].iface_narrow = element_is;                 /* …and the half the class cannot carry */
    DCHECK(!strcmp(NE_INIT[4].name, "destination") && !strcmp(NE_INIT[6].name, "formData") &&
           !strcmp(NE_INIT[11].name, "signal") && !strcmp(NE_INIT[12].name, "sourceElement"),
           "NavigateEventInit's member list moved under the four indices that carry its interface classes — the "
           "list is in Web IDL's conversion order, so inserting a member renumbers it and the brands would then "
           "be attached to the wrong members");
    prev = JS_GetClassProto(ctx, g_nav_ev_class);
    DCHECK(JS_IsNull(prev), "navigate_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "NavigateEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "NavigateEvent");
    for (i = 0; i < NE_N; i++)
        idl_install_accessor(ctx, proto, NE_NAME[i], js_ne_get, i, -1);
    /* §7.2.6.10.1's TWO OPERATIONS ARE NOT INSTALLED — see navigate_event.h. They are the interception half,
       and the IDL audit reports both as ABSENT rather than this file installing a shape for them. */
    JS_SetClassProto(ctx, g_nav_ev_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global. Its LENGTH is 2: Web IDL §3.7.4.1's length is the
       number of REQUIRED arguments, and this constructor's dictionary is not optional. */
    ctor = idl_step_constructor(ctx, "NavigateEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the NavigateEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "NavigateEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void navigate_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "HTML §7.2.6.10.1 The NavigateEvent interface was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ctor_stepid = -1;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_nav_ev_class = 0;
}
