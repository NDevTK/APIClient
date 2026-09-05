/* EVENTTARGET — DOM §2.7, over the objects this engine already hands the page.
 *
 * WPT NAMED THIS ONE. Every DOM test loads testharness.js, which declares its tests and then waits for the LOAD
 * EVENT before reporting — so with no event loop, 175 of 175 dom/nodes files ran the harness perfectly and
 * produced nothing at all. It is also how ordinary pages are written: the half of a bundle that runs on
 * DOMContentLoaded is the half that touches the DOM and calls the API.
 *
 * WHERE THE LISTENERS LIVE. On the TARGET, as an ordinary own property under a private key the page cannot
 * reach. That is not a shortcut — it is what makes registration per-flow for free: a listener added in one arm
 * of a fork is a property write like any other, so the COW delta captures it and the sibling never sees it.
 * A side table keyed by object pointer would have needed its own delta kind and its own swap, for the same
 * result.
 *
 * HOW A LISTENER RUNS. Not by JS_Call from C — a listener body is the page's code and holds loops, awaits and
 * concolic branches, so calling it from a C activation is the drive-to-completion the engine aborts on. Each
 * listener is dispatched through the promise machinery, which is how every other page callback in this engine
 * reaches a flow: the reaction runs as a call-root flow, preemptible and forkable like anything else. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"   /* §2.7's flattening decides C booleans out of values the page may not know */
#include "core/dom/abort.h"
#include "core/events/event.h"
#include "core/events/event_handler.h"
#include "core/events/event_path.h"
#include "core/events/mouse_event.h"   /* §2.9 step 6.4 asks a BRAND, and §6.5's click() fires a MouseEvent */
#include "core/events/report_exception.h"
#include "solver/result.h"   /* §3.2.15's refusal on a forked arm is this engine's own throw — see the AEL_SIGNAL stage */

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/events/event_target.h"

/* The private key the listener map hangs off. A SYMBOL, so a page enumerating its own objects cannot see it and
   cannot collide with it — the same reason the platform uses internal slots. */
/* `g_ready` rather than testing g_key: a static JSValue is ZERO-initialised, and zero is not JS_UNDEFINED — the
   tag is part of the value. Asking JS_IsUndefined of it answers "no" before anything has run, which fired the
   ran-twice assert on the FIRST call. A JSValue's emptiness is not the allocator's default. */
static JSValue g_key;
/* HTML §6.5 Activation behavior of elements' "Each element has an associated CLICK IN PROGRESS FLAG, which is
   initially unset" — the key that flag hangs off, a Symbol for the same reason the listener map's is: it is an
   internal slot, so a page can neither read it nor collide with it.
   ON THE WRAPPER AND NOT ON THE NODE, which is what makes it TIME-TRAVEL for free. The flag is set by step 3
   and unset by step 5 with the whole of step 4's dispatch in between, and that dispatch parks — so the flag is
   live across a suspension and must belong to the FLOW that raised it. Written as a property on the element's
   wrapper it is an ordinary slot write the COW delta already captures, and a sibling arm forked mid-click sees
   its own value; written into the lexbor node it would be one flag for every flow at once, which is the
   opposite of what a re-entrancy guard on a forking engine has to be. There is exactly one wrapper per node
   (core/dom/node.c's identity map), so `event.target.click()` and `el.click()` reach the same slot. */
static JSValue g_click_flag_key;
static int g_ready;
/* §2.9's PROPAGATION PATH IS THE TREE'S QUESTION, not this component's. Naming node.c here made EVERY host that
   installs events link the whole DOM and lexbor with it, which is why the streams gate could not build with a
   real AbortSignal in it. The tree registers the walk; a host that registers none has a path of one, which is
   exactly §2.7's own get the parent and exactly right for a host with no document — and, for the same reason,
   no shadow trees, which is the default every question below answers with. */
static const EventTargetTree *g_tree;
/* DOM §2.9's ACTIVATION BEHAVIOUR, declared by whoever owns the element — see event_target.h. */
static bool (*g_has_activation)(JSContext *ctx, JSValueConst el);
static int (*g_run_activation)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                               uint8_t *phase, uint32_t *req);
/* HTML §6.5 Activation behavior of elements' click() STEP 1's question, declared by whoever owns form controls
   — see event_target_set_click_terms. */
static bool (*g_is_disabled_form_control)(JSContext *ctx, JSValueConst el);
/* HTML §8.1.8.1's handler map key, and the MARKER that holds the handler's place in a listener list — see the
   event-handler section below. Declared here because event_target_init mints them. */
static JSValue g_handler_key;
static JSValue g_handler_marker;
/* HTML §8.1.8.1's INTERNAL RAW UNCOMPILED HANDLER — the third thing an event handler's `value` can be, beside
   null and a callback object, and the one a CONTENT attribute writes. It is a private-Symbol-keyed own slot on
   an engine-minted record, for the reason the handler map itself is one: the page cannot forge the key, so a
   record and a page-assigned object are told apart by a fact the page has no way to state. The alternative —
   storing the body as a bare string and reading "a string here means uncompiled" — would rest on Web IDL
   §3.2.20's non-object-to-null conversion happening in another function, and the day an entry point stored a
   string without it a page's `onclick` would be COMPILED AS SOURCE. The key holds the BODY, so the brand and
   the datum are one slot and a record with the brand and no body cannot exist. */
static JSValue g_uncompiled_key;
/* §9.4.2's handler-set hook — see event_target_set_handler_hook, far below, for what it is FOR. It is DEFINED
   here, beside the other three slots another component claims, because event_target_init declares all four to
   core/agent_state.h and a declaration needs the address. */
static void (*g_handler_set_hook)(JSContext *ctx, JSValueConst target, const char *name);
/* HTML §8.1.8.1's DEFINED TERMS — the six questions that section's algorithms ask of the tree and the HTML
   layer answers. Declared here beside the other slots another component claims, for the same reason they are. */
static const EventHandlerTerms *g_handler_terms;
/* The DISPATCH_PAIR step declaration — the one door a C caller has into the §2.9 machine. The FUNCTION OBJECT
   is minted per fire, in the FIRING REALM, and is never installed anywhere the page can reach: a C function
   runs in the realm that DEFINED it (js_call_c_function does `ctx = p->u.cfunc.realm`), so one object held in a
   static would have carried the agent realm's ctx into every child document's dispatch — and dispatch_path
   reads §7.6's window off that ctx, so a child document's `load` would have propagated to the ROOT window. */
static int g_dispatch_pair_stepid = -1;
/* The ids JS_RegisterStepDef handed this runtime for add/removeEventListener. `type` is a Web IDL DOMString,
   so it is ToString on whatever the page passed and cannot be a JS_ToCString from C. */
static int g_add_stepid = -1, g_remove_stepid = -1, g_dispatch_stepid = -1;
/* §8.1.8.1's TWO ACCESSORS PER ATTRIBUTE are declared with the handler table they are indexed by — see
   handler_declare_members, far below — because the table is what gives an index its meaning. */
/* §2.9's SYNTHETIC CLICK, whose declaration is beside its own step def far below. The ID is here with the
   other four because event_target_init declares all five to core/agent_state.h and the release gives all five
   back, and a slot named in one place and defined in another is how one of them came to be left set. */
static int g_click_stepid = -1;
/* §2.7's INTERFACE PROTOTYPE OBJECT. addEventListener, removeEventListener and dispatchEvent live HERE and
   nowhere else: every interface that inherits EventTarget — Node, AbortSignal, MessagePort, BroadcastChannel,
   Window — reaches them by CHAINING to it. They used to be installed onto each of those prototypes in turn,
   which is five copies of three members and, worse, a lie the corpus checks directly: `Node.prototype` is not
   where `addEventListener` is declared, `EventTarget.prototype` is, and `document instanceof EventTarget` is
   false when the interface does not exist at all.
   IT IS PER REALM, in quickjs's own per-context class-proto slot — the same place window.c keeps
   Window.prototype and bar_prop.c keeps BarProp.prototype, and for the same reason: js_call_c_function does
   `ctx = p->u.cfunc.realm`, so a member installed once answers every realm's question with the DEFINING realm's
   ctx forever. Here that is not an identity nicety, it is a wrong ANSWER — §3.6's [Global] rule resolves an
   unqualified `addEventListener('load', f)` against the RELEVANT GLOBAL, which this file reads off `ctx`, so a
   shared prototype registered every iframe's listeners on the ROOT window. A class id is what gives the slot a
   key; the class also brands `new EventTarget()`. */
static JSClassID g_et_class;
/* §2.7's TWO REGISTRATION MEMBERS ARE ONE STEP MACHINE, written beside the algorithm it is, far below. What is
   named here is the DECLARATION OBJECT itself rather than a body function: a step machine's rest points hang
   off the declaration, so the runtime is handed the one object and there is no second copy of it to disagree
   with the first. A tentative definition is what lets the declaration sit with its stages and still be the
   thing this function passes to the pool. */
static const IdlStepDecl AEL_DECL;
/* HTML §8.1.8.1's event handler getter machine, for the same reason and by the same tentative definition: the
   definition sits beside the algorithm and its stage list, and event_target_init declares it to the pool. */
static const IdlStepDecl EHG_DECL;
/* §8.1.8.1's ~90 attributes DECLARED — both accessors of every row, into the one args pool. It is a function
   rather than a loop in event_target_init because the ids are indexed by the handler table and belong beside
   it, and EH_COUNT is not in scope this far up the file. */
static void handler_declare_members(JSContext *ctx);
static void event_target_install(JSContext *ctx);

/* §8.1.8.2's handler list is AGENT state, so its two hand-written columns are checked where it is declared. */
static void eh_assert_types(void);

/* ---- §2.7's TWO OPTIONS DICTIONARIES, DECLARED ------------------------------------------------------------
 *
 * DOM §2.7 Interface EventTarget writes them as an inheritance pair:
 *
 *     dictionary EventListenerOptions { boolean capture = false; };
 *     dictionary AddEventListenerOptions : EventListenerOptions {
 *       boolean passive;
 *       boolean once = false;
 *       AbortSignal signal;
 *     };
 *
 * THE READ ORDER IS §3.2.17'S AND THE `level` COLUMN IS WHAT MAKES THE INHERITANCE PART OF THE DECLARATION.
 * Web IDL §3.2.17 Dictionary types builds "a list consisting of D and all of D's inherited dictionaries, in
 * order from least to most derived" and then reads "For each dictionary member member declared on dictionary,
 * in lexicographical order" — so `capture` is EventListenerOptions' (level 0) and the other three are this
 * dictionary's own (level 1). Declared flat, the list happened to agree because c < o < p < s; it agreed by
 * arithmetic and said nothing, and idl_dict_order_check checks the statement rather than the coincidence.
 *
 * `passive` IS IDL_BOOLEAN_NO_DEFAULT, NOT IDL_ANY. The reason that stood here was that IDL_BOOLEAN turns an
 * absent member into false while §2.7's flatten more options has to know whether the page WROTE it — true of
 * IDL_BOOLEAN, and exactly what IDL_BOOLEAN_NO_DEFAULT exists to say, so what the reason described was a
 * missing TYPE rather than an undeclarable member. Under it an absent `passive` stays undefined (the walk
 * exempts only IDL_BOOLEAN from the absent-member rewrite, and this is not that type), so flatten more
 * options step 4.2's `exists` test is intact; a value the page DID write is converted by the declaration
 * rather than by a JS_ToBool in the body.
 *
 * `signal` IS IDL_INTERFACE. The reason that stood here was that a signal's only brand was its private slot
 * record, which a body can test and a declaration comparing class ids cannot — WAS TRUE AND IS NOT:
 * core/dom/abort.c mints every signal through JS_NewObjectProtoClass against its own class and says so at the
 * mint, and idl_is_iface compares exactly that class id. It is the most dangerous kind of stale reason,
 * because it said a thing was impossible and so told the reader not to look; it had already been read as
 * current and relayed to another lane as a live defect.
 *   NULL IS NOT WHAT KEEPS IT OUT OF THE DECLARATION EITHER, and the body used to say it was. `AbortSignal` is
 *   NOT nullable, so `{signal: null}` is a TypeError — and the un-nullable IDL_INTERFACE is what produces one:
 *   Web IDL §3.2.17 Dictionary types step 4.1.4 is "If jsMemberValue is not undefined, then: Let
 *   idlMemberValue be the result of converting jsMemberValue to an IDL value whose type is the type member is
 *   declared to be of", so a present `null` is converted by Web IDL §3.2.15 Interface types, whose two steps
 *   are "If V implements I, then return the IDL interface type value that represents a reference to that
 *   platform object." and "Throw a TypeError."
 *   ABSENCE still crosses untouched (the walk rewrites an undefined member to IDL_ANY before any conversion),
 *   which is the null the algorithm means, and UNKNOWN EXTERNAL INPUT still crosses as itself, which is why
 *   the AEL_SIGNAL stage's three-arm fork survives this declaration rather than being collapsed by it.
 *
 * THE CLASS IS WRITTEN AT THE REALM INSTALL AND NOT HERE, for the reason core/events/navigate_event.c's
 * NavigateEventInit states at the identical seam: a class id is a RUNTIME registration made in
 * core/platform.c's row order, and this component is declared BEFORE `abort`, so at this line the id is still
 * zero. The two rows cannot simply swap, because AbortSignal.prototype chains to the prototype this file
 * builds and core/realm.h runs the per-realm installs in declaration order. A class id is agent-scoped rather
 * than per realm, so reading it at the first realm's install reads the id every later realm would and writing
 * it again writes the same value — which is why this is not the module static §per-realm-fact forbids. */
static IdlDictMember ADD_OPTS[] = {   /* `capture` FIRST: it is what the bare boolean means, and it is level 0 */
    { "capture", IDL_BOOLEAN,            false, NULL, 0 },
    { "once",    IDL_BOOLEAN,            false, NULL, 1 },
    { "passive", IDL_BOOLEAN_NO_DEFAULT, false, NULL, 1 },
    { "signal",  IDL_INTERFACE,          false, NULL, 1 },   /* .iface filled by event_target_install */
};
/* The one member of ADD_OPTS whose type names a class. Named rather than spelled at the write, because the
   list is in §3.2.17's read order and inserting a member renumbers it. */
enum { ADD_OPTS_SIGNAL = 3 };
static const IdlDictMember REMOVE_OPTS[] = { { "capture", IDL_BOOLEAN, false, NULL, 0 } };

void event_target_init(JSContext *ctx)
{
    JSClassDef d = { "EventTarget" };

    DCHECK(!g_ready, "event_target_init ran twice — one instance is one document");
    eh_assert_types();
    g_key = JS_NewSymbol(ctx, "eventListeners", false);
    CHECK(!JS_IsException(g_key), "the event-listener key allocation failed");
    g_click_flag_key = JS_NewSymbol(ctx, "clickInProgress", false);
    CHECK(!JS_IsException(g_click_flag_key), "HTML §6.5's click in progress flag key allocation failed");
    g_handler_key = JS_NewSymbol(ctx, "eventHandlers", false);
    g_handler_marker = JS_NewObject(ctx);
    g_uncompiled_key = JS_NewSymbol(ctx, "internalRawUncompiledHandler", false);
    CHECK(!JS_IsException(g_handler_key) && !JS_IsException(g_handler_marker) &&
          !JS_IsException(g_uncompiled_key),
          "the event-handler key, marker or uncompiled-handler brand allocation failed");
    g_ready = 1;
    {
        /* (DOMString type, EventListener? callback, optional (AddEventListenerOptions or boolean) options) —
           removeEventListener's third is (EventListenerOptions or boolean), which is the same union with only
           `capture` in it, and reading a member the IDL does not declare there simply never happens.
           NOT `if (g_add_stepid < 0)`. The test could only ever be true, because the assert at the top of this
           function says this declaration happens once per agent — and what it could DO was hand a SECOND agent
           the ids a dead runtime issued, which is core/agent_state.h's fetch defect exactly. The release below
           gives them back and the registry asserts that it did. */
        /* `EventListener? callback` IS A CALLBACK INTERFACE, WHICH IS NOT IDL_CALLBACK AND WAS NOT `any`.
           Web IDL §3.2.16 Callback interface types is two steps — "If V is not an Object, then throw a
           TypeError", then "Return the IDL callback interface type value that represents a reference to V,
           with the incumbent settings object as the callback context" — so ANY object crosses (its one
           operation is looked up by name at invoke time, which is why `{handleEvent(e){…}}` registers) and
           only a PRIMITIVE is refused. §3.2.19 Callback function types is the other type, the one that brands
           for callable, and declaring this position as that would reject the ordinary handleEvent object.
           IT IS THE DECLARATION'S BECAUSE §3.6 CONVERTS FROM LEFT TO RIGHT — "the JavaScript values are
           converted from left to right" — and the position AFTER it is a dictionary whose members are the
           PAGE'S reads. Performed in the body, this refusal ran after the whole options bag had been walked:
           `t.addEventListener("x", 5, {get capture(){ … }})` ran that getter, and over unknown external input
           it also forked §3.2.25's `(AddEventListenerOptions or boolean)` arm into two worlds that both then
           threw. Declared, argument 1 refuses first and neither happens. */
        static const IdlArgType ADD_ARGS[3] = { IDL_DOMSTRING, IDL_CALLBACK_INTERFACE_NULLABLE,
                                                IDL_DICT_OR_BOOL_FIRST };
        g_add_stepid    = idl_method_id_step(ctx, ADD_ARGS, 3, ADD_OPTS,
                                             (int)(sizeof(ADD_OPTS) / sizeof(ADD_OPTS[0])),
                                             &AEL_DECL, 0);
        idl_optional_from(2);   /* §2.7: `addEventListener(type, callback, optional options)` */
        g_remove_stepid = idl_method_id_step(ctx, ADD_ARGS, 3, REMOVE_OPTS,
                                             (int)(sizeof(REMOVE_OPTS) / sizeof(REMOVE_OPTS[0])),
                                             &AEL_DECL, 1);
        idl_optional_from(2);   /* §2.7: `removeEventListener(type, callback, optional options)` */
    }
    /* §8.1.8.1's ~90 attributes, BOTH ACCESSORS EACH, declared into the one args pool — which is what makes
       them ordinary installed members rather than a family defined at a raw JS_DefinePropertyGetSet. */
    handler_declare_members(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_et_class);
    JS_NewClass(JS_GetRuntime(ctx), g_et_class, &d);
    /* §2.7's PROTOTYPE IS A PER-REALM INTRINSIC LIKE EVERY OTHER ONE, and it goes in the same list — which is
       why this call is FIRST: the registry installs in declaration order, and every interface that inherits
       EventTarget chains to this realm's prototype while building its own. It was the one component whose
       install was hand-copied into each host's realm builder, which is the exact failure core/realm.h exists
       to end; a host cannot now forget it, because there is no line to forget. */
    realm_declare_intrinsic(event_target_install);
    agent_state_flag("event_target", &g_ready, "the declaration latch");
    agent_state_value("event_target", &g_key, "§2.7's listener-map key");
    agent_state_value("event_target", &g_click_flag_key, "HTML §6.5's click in progress flag key");
    agent_state_value("event_target", &g_handler_key, "HTML §8.1.8.1's handler-map key");
    agent_state_value("event_target", &g_handler_marker, "HTML §8.1.8.1's handler placeholder in a listener list");
    agent_state_value("event_target", &g_uncompiled_key, "HTML §8.1.8.1's internal raw uncompiled handler brand");
    agent_state_class("event_target", &g_et_class, "§2.7's interface prototype slot and brand");
    agent_state_id("event_target", &g_add_stepid, "§2.7's addEventListener machine");
    agent_state_id("event_target", &g_remove_stepid, "§2.7's removeEventListener machine");
    agent_state_id("event_target", &g_dispatch_stepid, "§2.7's dispatchEvent machine");
    agent_state_id("event_target", &g_dispatch_pair_stepid, "§2.9's internal dispatch machine");
    agent_state_id("event_target", &g_click_stepid, "§2.9's synthetic-click dispatch machine");
    /* THE FIVE SLOTS OTHER COMPONENTS CLAIM — four claims, since §2.9's activation behaviour is a PAIR. Each
       slot is this component's static and another component's obligation, so each is declared here, where the
       state lives, and cleared by whoever claimed it. The release below asserts all five are back, which is
       what puts the claimants BEFORE this row in core/platform.c's reverse-declaration order rather than
       leaving that ordering to be remembered. */
    agent_state_ptr("event_target", &g_tree, "§2.9's tree walk, claimed by core/dom/node.c");
    agent_state_ptr("event_target", &g_has_activation, "§2.9's activation predicate, claimed by core/html/hyperlink.c");
    agent_state_ptr("event_target", &g_run_activation, "§2.9's activation behaviour, claimed by core/html/hyperlink.c");
    agent_state_ptr("event_target", &g_handler_set_hook, "§9.4.2's handler-set hook, claimed by core/events/message_port.c");
    agent_state_ptr("event_target", &g_is_disabled_form_control,
                    "HTML §6.5's click() step 1 predicate, claimed by core/html/html_element.c");
}

/* §2.7's prototype FOR THIS REALM. Owned — the caller frees. */
JSValue event_target_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_et_class);
    DCHECK(JS_IsObject(proto),
           "EventTarget.prototype was asked for in a realm that never ran event_target_install — a realm whose "
           "intrinsics are half-built answers §2.7 with nothing");
    return proto;
}

/* §3.7.3's proto step for an interface declared `: EventTarget`, performed as §3.7.3 performs it — the parent
   is resolved FIRST and the object is OrdinaryObjectCreate'd over it, so the prototype never exists unchained.
   Owned — the caller frees. */
JSValue event_target_derived_proto(JSContext *ctx)
{
    JSValue etp = event_target_proto(ctx), proto = JS_NewObjectProto(ctx, etp);

    JS_FreeValue(ctx, etp);
    CHECK(!JS_IsException(proto),
          "the interface prototype object of an interface deriving from EventTarget could not be allocated");
    return proto;
}

/* §2.7 declares `constructor()`, so EventTarget IS constructible — `new EventTarget()` is a plain event target,
   which is what a page uses to give an ordinary object a listener list. It honours new.target's prototype, so a
   subclass gets its own. */
static JSValue js_event_target_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype"), obj;
    if (JS_IsException(proto)) return proto;
    /* §3.7.1: a subclass's `prototype` wins; without one it is THIS REALM's — and `ctx` is the CONSTRUCTOR's
       realm, which is the realm the interface object was installed in, so the two agree by construction. */
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = event_target_proto(ctx);
    }
    obj = JS_NewObjectProtoClass(ctx, proto, g_et_class);
    JS_FreeValue(ctx, proto);
    (void)argc; (void)argv;
    return obj;
}

void event_target_install_interface(JSContext *ctx, JSValueConst global)
{
    JSValue ctor = JS_NewCFunction2(ctx, js_event_target_ctor, "EventTarget", 0, JS_CFUNC_constructor, 0);
    JSValue proto = event_target_proto(ctx);
    int i;
    /* Web IDL §3.7.3's [Global] RULE REACHES THE INHERITED INTERFACES TOO. Window is declared [Global], and the
       rule is about the OBJECT, not about one interface: every member of every interface in the global's
       inheritance chain is an OWN property of the global. Window includes EventTarget, so
       `window.hasOwnProperty("addEventListener")` is true in every browser, and it is the SAME function object
       as `EventTarget.prototype.addEventListener` — the member is one declaration placed twice, never a second
       one. Reaching it up the chain is observably different: a page that copies the global's own property
       names, or reads a descriptor off `window`, sees nothing there. */
    static const char *const GLOBAL_MEMBERS[3] = { "addEventListener", "removeEventListener", "dispatchEvent" };

    CHECK(!JS_IsException(ctor), "the EventTarget interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    for (i = 0; i < 3; i++) {
        JSAtom a = JS_NewAtom(ctx, GLOBAL_MEMBERS[i]);
        JSValue fn = JS_GetProperty(ctx, proto, a);
        CHECK(JS_IsFunction(ctx, fn), "§2.7's prototype is missing a member the global must carry its own "
                                      "reference to — the two lists are one declaration read twice");
        /* Web IDL §3.7.6's flags for an operation, and all three are asserted by the corpus: writable,
           ENUMERABLE and configurable. An IDL member is enumerable — that is what makes a for-in over a
           platform object list the platform's own names — and only [LegacyUnforgeable] takes configurable
           away, which none of these three carry. */
        JS_DefinePropertyValue(ctx, (JSValue)global, a, fn, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "EventTarget", ctor);
}

void event_target_set_tree(const EventTargetTree *tree)
{
    /* NULL IS THE RELEASE, and it is the same call because the slot has one owner: the component that
       registered the walk gives it back at its own release, which must run BEFORE this one — see the assert in
       event_target_free, which is what makes that ordering a checked fact rather than a remembered one. */
    if (tree == NULL) {
        DCHECK(g_tree != NULL, "§2.9's tree walk was released by a component that never registered one");
        g_tree = NULL;
        return;
    }
    DCHECK(g_tree == NULL,
           "a second component registered §2.9's tree walk — there is ONE tree, and the second registration "
           "silently decides every propagation path the first was answering");
    DCHECK(tree != NULL && tree->get_parent != NULL && tree->default_passive_target != NULL &&
           tree->root != NULL && tree->shadow_root_mode != NULL && tree->is_window != NULL &&
           tree->is_slot != NULL && tree->is_assigned_slottable != NULL &&
           tree->is_shadow_including_inclusive_ancestor != NULL,
           "half a tree was registered with the events layer — §2.9's walk, §2.7's default passive value and the "
           "six shadow facts the walk composes are all tree questions, and one answered without the others is a "
           "component that silently never runs. It is ALL of them or none: a tree that answers the walk but not "
           "the shadow terms would build a path that crosses boundaries without retargeting at them");
    g_tree = tree;
}

bool event_target_is_window(JSContext *ctx, JSValueConst target)
{
    /* A host with no tree registered has no globals in anyone's propagation path either — the same answer
       dispatch_get_parent takes for the same reason, rather than a second question about what a global is. */
    return g_tree != NULL && g_tree->is_window(ctx, target);
}

void event_target_set_activation(bool (*has)(JSContext *ctx, JSValueConst el),
                                 int (*run)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                                            uint8_t *phase, uint32_t *req))
{
    DCHECK((has != NULL) == (run != NULL),
           "half an activation behaviour was registered — a predicate with nothing to perform picks an "
           "activation target the dispatch then cannot run, and a performer with no predicate is never picked");
    /* ONE CLAIMANT, AND NULL IS THE RELEASE — see event_target_set_tree for why the two are one call. */
    DCHECK(has == NULL || g_has_activation == NULL,
           "a second component registered §2.9's activation behaviour — there is one pair, and the second "
           "claim silently decides what every activation target does");
    DCHECK(has != NULL || g_has_activation != NULL,
           "§2.9's activation behaviour was released by a component that never registered one");
    g_has_activation = has;
    g_run_activation = run;
}

void event_target_set_click_terms(bool (*is_disabled_form_control)(JSContext *ctx, JSValueConst el))
{
    /* NULL IS THE RELEASE — see event_target_set_tree for why the two are one call. */
    if (is_disabled_form_control == NULL) {
        DCHECK(g_is_disabled_form_control != NULL,
               "HTML §6.5's click() step 1 predicate was released by a component that never registered one");
        g_is_disabled_form_control = NULL;
        return;
    }
    DCHECK(g_is_disabled_form_control == NULL,
           "a second component registered HTML §6.5's click() step 1 predicate — that step is \"If this "
           "element is a form control that is disabled, then return\", there is ONE answer to it, and a "
           "second claim silently decides which elements in the agent can be clicked at all");
    g_is_disabled_form_control = is_disabled_form_control;
}

void event_target_set_handler_terms(const EventHandlerTerms *terms)
{
    /* NULL IS THE RELEASE — see event_target_set_tree for why the two are one call. */
    if (terms == NULL) {
        DCHECK(g_handler_terms != NULL,
               "HTML §8.1.8.1's defined terms were released by a component that never registered them");
        g_handler_terms = NULL;
        return;
    }
    DCHECK(g_handler_terms == NULL,
           "a second component registered HTML §8.1.8.1's defined terms — there is ONE answer to `is this a "
           "body element`, and a second registration silently decides which object every `body.onload = f` in "
           "the agent lands on");
    DCHECK(terms->is_body_or_frameset != NULL && terms->node_document_is_active != NULL &&
           terms->node_document_global != NULL && terms->is_element != NULL &&
           terms->node_document != NULL && terms->form_owner != NULL,
           "half of §8.1.8.1's defined terms were registered — determine the target's steps 1, 3 and 4 are one "
           "algorithm, and a host that can say an element is a body but not whether its node document is "
           "active would delegate a handler to a Window that its own step 3 forbids reaching; get the current "
           "value's step 3.1 partition and step 3.9's scope are the other three, and a host that can name an "
           "element but not its node document would compile an inline handler with a scope chain missing the "
           "layer its own step 3.9 substep 3 adds");
    g_handler_terms = terms;
}

void event_target_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release
       row has a declare — so a release reaching here undeclared is a host tearing this component down with
       something that is not the platform's list. */
    DCHECK(g_ready, "§2.7's event machinery was released in an agent that never declared it");
    /* THE FIVE SLOTS OTHER COMPONENTS CLAIM ARE EMPTY BY NOW, AND THAT IS AN ORDERING STATEMENT. Each
       claimant holds a C function pointer INTO its own component; this row is about to give back the state
       those functions read, so a claimant still holding one is a component released AFTER the thing it points
       into. core/platform.c's reverse-declaration order is what puts node/hyperlink/message_port/html_element
       first, and this is where that order stops being something a reader has to reconstruct. */
    DCHECK(g_tree == NULL,
           "§2.9's tree walk was still registered when the event machinery was released — core/dom/node.c "
           "claimed it and must give it back at node_free, which reverse-declaration order runs first");
    DCHECK(g_has_activation == NULL && g_run_activation == NULL,
           "§2.9's activation behaviour was still registered when the event machinery was released — "
           "core/html/hyperlink.c claimed it and must give it back at hyperlink_free");
    DCHECK(g_handler_set_hook == NULL,
           "HTML §8.1.8.1's handler-set hook was still registered when the event machinery was released — "
           "core/events/message_port.c claimed it and must give it back at message_port_free");
    DCHECK(g_handler_terms == NULL,
           "HTML §8.1.8.1's defined terms were still registered when the event machinery was released — "
           "core/html/html_element.c claimed them and must give them back at "
           "html_element_free, which reverse-declaration order runs first");
    DCHECK(g_is_disabled_form_control == NULL,
           "HTML §6.5's click() step 1 predicate was still registered when the event machinery was released — "
           "core/html/html_element.c claimed it and must give it back at html_element_free, which "
           "reverse-declaration order runs first");
    JS_FreeValueRT(rt, g_key);
    JS_FreeValueRT(rt, g_click_flag_key);
    JS_FreeValueRT(rt, g_handler_key);
    JS_FreeValueRT(rt, g_handler_marker);
    JS_FreeValueRT(rt, g_uncompiled_key);
    /* THE PROTOTYPE IS NOT RELEASED HERE: each realm's is held by that realm's class-proto slot and goes with
       the realm. Neither is the dispatcher — there is no lasting one to hold. */
    /* EVERY HANDLE THIS COMPONENT DECLARED, GIVEN BACK, FROM THE ONE LIST THAT ALREADY NAMES THEM — the five
       values freed above, the class id, the six step ids, §8.1.8.1's ~90 accessor pairs and the declaration
       latch. A step id and a class id name a registration in the runtime that is going away with them; kept,
       they are what a SECOND agent's lazy `if (id < 0)` reads to decide it need not register again —
       core/agent_state.h's fetch defect.
       RETIRED TEXT, unquoted because it is this file's own and not a standard's: this was eight assignments
       enumerating the slots by hand, under a comment saying the registry asserts them. The enumeration was
       WRONG for as long as §8.1.8.1's getter machine had a declaration in event_target_init and no line here,
       and it aborted the two build stages that provision a second agent. It is deleted rather than corrected,
       because a list of slots kept in step by whoever remembers is the defect and the missing line was only
       its symptom — see core/agent_state.h's agent_state_undo for the argument.
       IT IS LAST because the five DCHECKs above ask whether a claimant has handed its hook back, and those
       hooks are slots this call would otherwise null before they were asked about. */
    agent_state_undo("event_target");
}

/* THE RELEVANT GLOBAL OBJECT, WHICH IS THE RUNNING REALM'S — asked of the realm rather than remembered.
   A module-static held it, set by every realm's install, so the LAST document installed was the window every
   realm answered with: materializing a same-origin popup made the OPENER's unqualified `addEventListener(...)`
   register on the popup's global. That is the same defect as the API base URL having been one string for every
   realm, and the same defect as `window.name` having had two sources — a per-realm fact answered per agent.
   There is nothing to set now, so there is nothing a host can forget to set; the DCHECK that caught a host
   forgetting goes with the state it was guarding.
   BORROWED. A realm owns its global for the realm's whole life, so this needs no reference of its own — the
   dup and free below are how quickjs spells "read it without taking one". */
static JSValueConst event_target_global(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_FreeValue(ctx, g);
    return g;
}

/* WEB IDL §3.6's [Global] RULE: an operation on the Window interface called with an undefined `this` uses the
   RELEVANT GLOBAL OBJECT. That is not a nicety — `addEventListener('load', init)` written unqualified is how a
   great deal of real code registers, and a bare call has an undefined this-binding, so without this rule every
   one of those listeners was registered on nothing at all and silently never fired. It is applied at the shared
   entry because that is where the receiver arrives; a non-global interface reached with undefined would
   otherwise be an immediate TypeError, so there is nothing here for this to take away. */
static JSValueConst event_target_receiver(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsObject(this_val)) return this_val;
    return event_target_global(ctx);
}

/* The map of type -> listener array on `target`, created on first use. NULL only on allocation failure. */
static JSValue listener_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-listener map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP. `window` is the global object, and a miss on the global is the
       solver's absent-state seam: it mints a concolic for the name a page read and did not define. That is
       right for the page's own reads and wrong for an internal slot — the map came back as a concolic, every
       listener was stored on it, and window.addEventListener silently registered nothing. An internal slot is
       by definition an own slot, so it is read as one. */
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = idl_slots_new(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* §2.7 A LISTENER IS NOT A CALLBACK — it is a RECORD: {callback, capture, once, passive}. Storing the bare
   callback made three of those four unrepresentable, so `once` fired every time (a real bundle's one-shot
   init ran on every event), `capture` had no phase, and the dedup key was wrong — the spec's key is
   (type, callback, capture), so the same function registered once capturing and once bubbling is TWO
   listeners and used to be silently one. The record is an engine-built null-prototyped object, so reading it
   back from C runs none of the page's code. */
static JSValue listener_record(JSContext *ctx, JSValueConst cb, bool capture, bool once, bool passive)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "event listeners: OOM recording a registration — a dropped listener is page "
                                "code that never runs");
    JS_SetPropertyStr(ctx, rec, "cb", JS_DupValue(ctx, cb));
    JS_SetPropertyStr(ctx, rec, "capture", JS_NewBool(ctx, capture));
    JS_SetPropertyStr(ctx, rec, "once", JS_NewBool(ctx, once));
    JS_SetPropertyStr(ctx, rec, "passive", JS_NewBool(ctx, passive));
    /* §2.7's REMOVED FIELD, and it is the fifth field for a reason the spec states in a note: dispatch walks a
       CLONE of the list so that a listener added mid-walk does not run, "note that removal still has an effect
       due to the removed field". Without it, removing a listener from inside a dispatch removed it from the live
       list and the snapshot ran it anyway — the one thing the clone must NOT preserve. */
    JS_SetPropertyStr(ctx, rec, "removed", JS_FALSE);
    return rec;
}

/* §2.7 "remove an event listener" step 2: SET REMOVED, then drop it. The two halves are one operation, and the
   first is what a dispatch already holding a snapshot of this list observes. */
static void listener_mark_removed(JSContext *ctx, JSValueConst rec)
{
    JS_SetPropertyStr(ctx, (JSValue)rec, "removed", JS_TRUE);
}

/* …AND READING ONE BACK ASSERTS WHAT WRITING IT PROMISED. Every flag on this record was written by
   listener_record out of a decided arm, so a value here that is not a BOOLEAN is a registration that carried an
   undecided one past §2.7's flattening — and ToBoolean would answer `true` for it and say nothing, which is the
   collapse the flattening stages exist to prevent. Asserting at the read is what makes the write's promise
   checkable from the other side. */
static bool rec_flag(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, name);
    bool b;

    DCHECK(JS_IsBool(v),
           "an event listener's flag is not a boolean — §2.7's listener record holds capture, once, passive and "
           "removed as booleans, and every one of them is written from an arm this engine has already decided");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

/* The record's callback, or the handler MARKER — an owned value either way. */
static JSValue rec_cb(JSContext *ctx, JSValueConst rec)
{
    return JS_GetPropertyStr(ctx, rec, "cb");
}

/* Does this record register `cb` with this capture flag? §2.7's identity, and the whole of it. */
static bool rec_matches(JSContext *ctx, JSValueConst rec, JSValueConst cb, bool capture)
{
    JSValue c = rec_cb(ctx, rec);
    bool same = JS_VALUE_GET_TAG(c) == JS_VALUE_GET_TAG(cb) && JS_VALUE_GET_PTR(c) == JS_VALUE_GET_PTR(cb);
    JS_FreeValue(ctx, c);
    return same && rec_flag(ctx, rec, "capture") == capture;
}

/* The live list for (target, type), created on demand. OWNED. */
static JSValue listener_list(JSContext *ctx, JSValueConst target, const char *type, int create)
{
    JSValue map = listener_map(ctx, target, create), arr;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr) && create) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, arr));
    }
    JS_FreeValue(ctx, map);
    return arr;
}

static uint32_t arr_len(JSContext *ctx, JSValueConst arr)
{
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static JSValue remove_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                         bool capture);

/* §2.7 step 6's ABORT ALGORITHM, as a closure over exactly the four things "remove an event listener" needs.
   It is an ALGORITHM and not an `abort` listener: §3.2 runs the algorithms BEFORE it fires `abort`, so a
   listener registered with a signal is already gone by the time the page's own `abort` handler runs — and an
   algorithm is invisible to the page, which cannot remove it or see it in a listener list.
   The captures are the REGISTRATION's identity, which is (target, type, callback, capture) and nothing else:
   holding the RECORD instead would keep a listener the page has since removed and re-added alive as a second
   entry to delete. */
static JSValue js_listener_signal_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic, JSValue *func_data)
{
    const char *type = JS_ToCString(ctx, func_data[1]);   /* a real string: the registration converted it */

    (void)this_val; (void)argc; (void)argv; (void)magic;
    if (type) {
        remove_listener_with_type(ctx, func_data[0], func_data[2], type, JS_ToBool(ctx, func_data[3]));
        JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}

static JSValue listener_signal_algorithm(JSContext *ctx, JSValueConst target, const char *type,
                                         JSValueConst cb, bool capture)
{
    JSValueConst data[4];
    JSValue t = JS_NewString(ctx, type), fn;

    data[0] = target; data[1] = t; data[2] = cb; data[3] = JS_NewBool(ctx, capture);
    fn = JS_NewCFunctionData(ctx, js_listener_signal_abort, 0, 0, 4, data);
    JS_FreeValue(ctx, t);
    CHECK(!JS_IsException(fn), "the abort algorithm for a signal-scoped listener could not be allocated — a "
                               "dropped one is a listener that outlives the signal that owns it");
    return fn;
}

/* §2.7's DEFAULT PASSIVE VALUE, given an event type and an EventTarget. The four types are this file's list —
   they are named in the spec, not derived — and the target test is the tree's. Both halves must hold. */
static bool default_passive_value(JSContext *ctx, const char *type, JSValueConst target)
{
    static const char *const PASSIVE_BY_DEFAULT[] = { "touchstart", "touchmove", "wheel", "mousewheel", NULL };
    int i;

    for (i = 0; PASSIVE_BY_DEFAULT[i]; i++)
        if (!strcmp(type, PASSIVE_BY_DEFAULT[i]))
            return g_tree != NULL && g_tree->default_passive_target(ctx, target);
    return false;
}

/* The listener-list work, once `type` is a real string. Split from the coercion so the part that CAN reach the
   page's code is a request and the part that cannot is ordinary C.
   `passive` is a TRISTATE, because §2.7's flatten more options makes it one: -1 means the page did not say, and
   step 4 of "add an event listener" then fills it from the default passive value. Collapsing it to false at the
   dictionary read would make `{passive:false}` and `{}` the same registration, which for a wheel listener on
   the window is exactly the difference the flag exists to express. */
static JSValue add_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                      bool capture, bool once, int passive, JSValueConst signal)
{
    JSValue arr;
    uint32_t len, i;

    /* §2.7 step 2: a listener registered with an ALREADY-ABORTED signal is not registered at all. */
    if (abort_signal_is(ctx, signal) && abort_signal_aborted(ctx, signal))
        return JS_UNDEFINED;
    if (passive < 0)
        passive = default_passive_value(ctx, type, this_val);
    arr = listener_list(ctx, this_val, type, 1);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return JS_UNDEFINED; }
    len = arr_len(ctx, arr);
    /* "If the event listener list already contains a listener whose type, callback and capture are the same,
       do nothing." The flags of the EXISTING one win — a second add does not change `once`. */
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        bool same = JS_IsObject(e) && rec_matches(ctx, e, cb, capture);
        JS_FreeValue(ctx, e);
        if (same) { JS_FreeValue(ctx, arr); return JS_UNDEFINED; }
    }
    JS_SetPropertyUint32(ctx, arr, len, listener_record(ctx, cb, capture, once, passive != 0));
    JS_FreeValue(ctx, arr);
    /* §2.7 step 6: "If listener's signal is non-null, then add the following abort steps to it: remove an event
       listener with eventTarget and listener." An ABORT ALGORITHM, which is what §3.2 calls a piece of engine
       work that runs BEFORE the `abort` event and is invisible to the page — a page-visible `abort` listener
       would be one the page could see, remove, or have run out of order with its own. */
    if (abort_signal_is(ctx, signal)) {
        JSValue algo = listener_signal_algorithm(ctx, this_val, type, cb, capture);
        abort_signal_add_algorithm(ctx, signal, algo);   /* BORROWED: the signal takes its own reference */
        JS_FreeValue(ctx, algo);
    }
    return JS_UNDEFINED;
}

static JSValue remove_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                         bool capture)
{
    JSValue map, arr, kept;
    uint32_t len, i, k = 0;

    map = listener_map(ctx, this_val, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    len = arr_len(ctx, arr);
    kept = JS_NewArray(ctx);
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        bool same = JS_IsObject(e) && rec_matches(ctx, e, cb, capture);
        if (same) { listener_mark_removed(ctx, e); JS_FreeValue(ctx, e); }
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
    return JS_UNDEFINED;
}

void event_target_add_listener(JSContext *ctx, JSValueConst target, const char *type, JSValueConst cb,
                               bool capture, bool once, int passive, JSValueConst signal)
{
    JS_FreeValue(ctx, add_listener_with_type(ctx, event_target_receiver(ctx, target), cb, type,
                                             capture, once, passive, signal));
}

void event_target_remove_listener(JSContext *ctx, JSValueConst target, const char *type, JSValueConst cb)
{
    JS_FreeValue(ctx, remove_listener_with_type(ctx, event_target_receiver(ctx, target), cb, type,
                                                /*capture*/ false));
}

/* §2.7's event listener list, asked as "is it non-empty for ANY type". The map is this file's own engine-built
   object, so walking it runs none of the page's code — and a handler ATTRIBUTE is in it because setting one
   registers a real listener, which is the whole reason the caller must not count for itself. */
bool event_target_has_any_listener(JSContext *ctx, JSValueConst target)
{
    JSValue map = listener_map(ctx, event_target_receiver(ctx, target), 0);
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    bool any = false;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return false; }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, map, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (i = 0; i < n && !any; i++) {
            JSValue arr = JS_GetProperty(ctx, map, tab[i].atom);
            if (JS_IsArray(arr) && arr_len(ctx, arr) > 0) any = true;
            JS_FreeValue(ctx, arr);
        }
        for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, map);
    return any;
}

/* DOM §2.7 Interface EventTarget's "ERASE ALL EVENT LISTENERS AND HANDLERS, given an EventTarget eventTarget":
 * "1. Remove all of eventTarget's event listeners. 2. Set eventTarget's event handler map's entries to null."
 * HTML §8.4.1 "Opening the input stream" is the caller — it erases the whole document tree's and the Window's,
 * which is what stops a `load` handler that wrote the document from running again the moment the write closes.
 *
 * IT REPLACES EACH MAP RATHER THAN DELETING THE SLOT, and the reason is the per-flow delta rather than style:
 * a fresh empty map is a property WRITE, which the heap COW captures and a context switch unapplies, so one
 * flow's `document.open()` does not erase a sibling's listeners. A delete would have to be a capture of a
 * different shape, and one that is silently absent leaves the erase permanent for every flow.
 * ONE CALL CLEARS BOTH, because a handler is in BOTH maps: §8.1.8.1 "Event handlers" stores the handler in the
 * handler map and registers `g_handler_marker` in the listener list to hold its position, so clearing either
 * alone leaves half a handler — a marker that resolves to nothing, or a handler nothing dispatches to. */
void event_target_erase_all(JSContext *ctx, JSValueConst target)
{
    JSValue k[2];
    int i;

    DCHECK(g_ready, "event listeners were erased before the keys existed");
    DCHECK(JS_IsObject(target),
           "erase all event listeners and handlers was given something that is not an object — §2.7's "
           "algorithm is stated over an EventTarget, and the maps it clears are OWN SLOTS on one");
    k[0] = g_key;
    k[1] = g_handler_key;
    for (i = 0; i < 2; i++) {
        JSAtom a = JS_ValueToAtom(ctx, k[i]);
        JSValue empty;

        if (a == JS_ATOM_NULL) continue;
        empty = idl_slots_new(ctx);
        CHECK(!JS_IsException(empty),
              "erase all event listeners and handlers could not allocate the empty map that replaces the old "
              "one — leaving the page's listeners standing on a document §8.4.1 has already emptied");
        JS_SetProperty(ctx, (JSValue)target, a, empty);
        JS_FreeAtom(ctx, a);
    }
}

/* add/removeEventListener's `type` is a Web IDL DOMString, so it is ToString on whatever the page passed and
   cannot be a JS_ToCString from C. They use the SHARED coerce-then-call machine rather than one of their own:
   what they have in common with getAttribute and createElement is exactly the thing that needs a machine, and a
   second copy is a second chance to get the resumption wrong.
   IT ALSO FIXES AN ORDERING MISTAKE I MADE. A bespoke version checked 2.7's "if callback is null, return"
   BEFORE the coercion, to avoid running a toString for a call that does nothing. That is backwards: Web IDL
   converts arguments in ORDER at call time, so `type` is converted first and the null-callback step is part of
   the algorithm that runs after. `addEventListener({toString(){ … }}, null)` DOES run that toString in a real
   browser, and now here.
 *
 * AND THE FLATTENING TURNS UNKNOWN EXTERNAL INPUT INTO C BOOLEANS, WHICH IS WHERE THE WORLDS ARE DECIDED.
 * DOM §2.7 Interface EventTarget's flatten options step 2 reads `capture` and its flatten more options steps
 * 4.1-4.3 read `once`, `passive` and `signal`; every one of them becomes a C `bool` (or a brand test) that this
 * algorithm then branches on. A page's options bag comes from wherever the page got it — a fetched config, a
 * bag parsed out of `location.hash` — so any of those members can be unknown external input, and Web IDL
 * §3.2.17 Dictionary types step 4.1.4's conversion deliberately CROSSES it as itself (idl_args.h's
 * IDL_CONCOLIC_CROSSES) so the taint survives the boundary and arrives HERE with its domain intact.
 * ToBoolean AT THIS BOUNDARY IS THE COLLAPSE THAT CROSSING EXISTS TO PREVENT. A concolic wears an Object —
 * solver/concolic.c gives it one so a method on an unknown yields another unknown instead of throwing — and
 * every Object is TRUTHY, so the plain reader answered `capture:true`, `once:true` and `passive:true` for every
 * unknown there has ever been, pinning three flags to one world apiece. That is the same defect §3.2.25 Union
 * types' arm had one level up, and while it stood it made the arm fork half-useless: both arms of the union ran
 * and then each arm collapsed its members anyway.
 * SO EACH MEMBER IS A FORK, AND A FORK IS WHY THIS IS A MACHINE. decide.h states the reason in its own words —
 * a plain C body is already inside its activation when it asks and has no state for the other arm to be
 * snapshotted at, and the declaration to build is the step machine. Each question is its own STAGE, because a
 * machine may have exactly one request outstanding and a stage that asked twice would re-ask the first on every
 * re-entry and never converge; the sibling resumes AT the stage that asked and re-derives its arm there.
 * OUTCOME 0 IS `false` FOR EVERY BOOLEAN MEMBER, AND THE IDL SAYS SO RATHER THAN THIS ENGINE'S OLD ANSWER.
 * §2.7 declares `boolean capture = false` on EventListenerOptions and `boolean once = false` on
 * AddEventListenerOptions, so false is what the conversion produces when the page writes nothing and is the
 * registration this member performs by default; `passive` declares no default, and false is the arm on which a
 * listener may still call preventDefault, which is the listener the platform had before the flag existed. The
 * mapping is FIXED and is deliberately not the concolic's example: a decision vector records ARMS, and a
 * resumed flow re-derives its example values from CURRENT sources, so an example-keyed arm would name a
 * different boolean in the next session than in this one — the cross-session divergence a recorded path must
 * not have.
 * AND THE NUMBER OF ARMS IS PER MEMBER, NOT PER TYPE — WHICH IS WHY `passive` HAS NO ENTRY IN THIS LIST.
 * `capture` and `once` declare `= false`, so Web IDL §3.2.17 Dictionary types step 4.1.5 makes their ABSENT
 * world their false world and two arms exhaust them; that is a fact about those two declarations, and it is
 * the whole of what makes a two-armed ask complete for them. `passive` declares no default, so absent is a
 * third answer flatten more options step 4.2's `exists` test distinguishes and add an event listener step 4
 * fills differently — and that answer is §3.2.17 step 4.1.4's, decided by the member loop at the outcome seam
 * before §3.2.3 is asked, not by a stage in this file. The AEL_PASSIVE stage asked it here for as long as the
 * loop crossed a no-default boolean read off an unknown source; it does not, so there is no `passive`
 * operation string and nothing in this file forks over that member. */
#define AEL_OP_CAPTURE "DOM §2.7 flatten options `capture`"
#define AEL_OP_ONCE    "DOM §2.7 flatten more options `once`"
#define AEL_OP_SIGNAL  "DOM §2.7 flatten more options `signal`"

/* WHERE THIS MACHINE RESTS, AS THE TWO STANDARDS NUMBER THEM. One member serves addEventListener and
   removeEventListener — they are one algorithm with one of them stopping earlier, which is what the magic
   selects — so removeEventListener enters AEL_CAPTURE and leaves for AEL_RUN, and the three stages between
   them belong to steps its own algorithm does not have. */
#define AEL_STAGES(X)                                                                                            \
    X(AEL_ENTER,   "Web IDL §3.2.16 Callback interface types step 1 (argument 1's TypeError, which precedes "     \
                   "every read of the options because Web IDL converts arguments in order)")                     \
    X(AEL_CAPTURE, "DOM §2.7 Interface EventTarget, flatten options step 2 (`capture`)")                          \
    X(AEL_ONCE,    "DOM §2.7 Interface EventTarget, flatten more options step 4.1 (`once`)")                      \
    X(AEL_PASSIVE, "DOM §2.7 Interface EventTarget, flatten more options step 4.2 (`passive`), whose `exists` "   \
                   "test and whose two booleans are the three feasible completions of one unknown")              \
    X(AEL_SIGNAL,  "DOM §2.7 Interface EventTarget, flatten more options step 4.3 (`signal`), whose Web IDL "     \
                   "§3.2.15 Interface types conversion and whose add an event listener step 2 aborted test are "  \
                   "the feasible completions of one unknown")                                                    \
    X(AEL_RUN,     "DOM §2.7 Interface EventTarget, add an event listener steps 3-6 / remove an event listener "  \
                   "step 2 (the listener-list work, once every conversion has an answer)")
enum { IDL_STEP_STAGE_BASE(AEL_STAGES) AEL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const AEL_STEPS[] = { AEL_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §2.7's flattening, as the four answers it produces. The three flags are PLAIN BYTES and not JSValues on
   purpose: the standard's own listener record holds booleans, and a byte crosses the fork's clone and the cold
   tier without owning anything. `passive` is a TRISTATE because flatten more options step 4.2 makes it one —
   -1 is "the member does not exist", which add an event listener step 4 then fills from the default passive
   value, and collapsing it to false at the read would make `{passive:false}` and `{}` the same registration,
   which for a wheel listener on a Window is exactly the difference the flag exists to express. */
typedef struct {
    uint8_t capture;
    uint8_t once;
    int8_t  passive;
    /* Step 4.3's signal once §3.2.15 has answered for it (owned): the page's own AbortSignal, or the one this
       machine builds for the arm on which the unknown IS a live signal. JS_UNDEFINED means the member is
       absent, which is the null the algorithm means. */
    JSValue signal;
} AelState;

static void ael_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    AelState *s = st;

    v->val(ctx, &s->signal);
}

/* ONE BOOLEAN MEMBER OF THE FLATTENING WHOSE ABSENT WORLD IS ITS FALSE WORLD, ANSWERED WHERE IT BECOMES A C
   bool — `capture` and `once`, and deliberately NOT `passive`.
   TWO ARMS IS COMPLETE HERE ONLY BECAUSE OF THE DEFAULT, and that is a fact about those two members rather than
   about booleans. Web IDL §3.2.17 Dictionary types step 4.1.5 gives an absent member "member's default value",
   and DOM §2.7 Interface EventTarget declares `boolean capture = false` on EventListenerOptions and
   `boolean once = false` on AddEventListenerOptions — so "the page did not write it" and "the page wrote false"
   are the SAME registration and there is no third world to ask for. `boolean passive;` declares no default, so
   its absent world is null and observably its own; the AEL_PASSIVE stage asks the three-completion question
   itself rather than reaching for this helper.
   `v` IS BORROWED AND IS DELIBERATELY NOT HELD ACROSS THE RETURN, AND WHOSE REFERENCE KEEPS IT ALIVE IS THE
   POINT. step_fork_run leaves a borrowed pointer to it on the header, which the driver reads after this
   machine has returned — and what it must NOT be borrowed from is the `argv` this body was handed, because
   that vector is a transient copy the conversion machine frees the instant the body returns. It is borrowed
   from the CONVERTED DICTIONARY instead: the dictionary is the declared position's own converted value, which
   js_idl_args_visit names twice over (`conv` and the per-position vector), so it outlives the return, the
   fork's clone carries it, and the sibling reads the same member off the same object when it resumes here. */
static int ael_flag_run(JSContext *ctx, JSStepHdr *hdr, JSValueConst v, const char *op, uint8_t *out)
{
    int arm = 0, rc;

    if (!concolic_is(v)) {
        *out = JS_ToBool(ctx, v) ? 1 : 0;
        return 0;
    }
    rc = step_fork_run(ctx, hdr, v, op, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
    if (rc) return rc;
    DCHECK(arm == 0 || arm == 1,
           "§2.7's flattening of `capture` or `once` came back on an arm that is neither of the two a boolean "
           "has. Two is all these two members have BECAUSE THEY DECLARE `= false`, so §3.2.17 step 4.1.5 makes "
           "their absent world their false world — a member with no default owes a third arm and asks for it "
           "at its own stage, never here");
    *out = (uint8_t)arm;
    return 0;
}

static int ael_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    AelState *s = st;
    const int magic = idl_step_magic(hdr);
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue v, r;
    const char *type;
    int rc;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == AEL_ENTER) {
        /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the throw below tears this state
           down through ael_visit, which names exactly what the state owns and nothing else, so a field handed
           over after it would be freed by nobody. */
        s->signal = JS_UNDEFINED;
        s->capture = 0;
        s->once = 0;
        s->passive = -1;
        /* WEB IDL CONVERTS EVERY ARGUMENT, IN ORDER, BEFORE THE ALGORITHM RUNS — so the shape of this machine
           is "finish the conversions, THEN run §2.7", and the two halves are not interleavable. §2.7's
           `callback` is an `EventListener?`, which is a CALLBACK INTERFACE and not a function type: ANY object
           implements it, because its one operation is looked up BY NAME on the object each time it is invoked,
           so `el.addEventListener("x", {handleEvent(e){…}})` is an ordinary registration.
           THE REFUSAL THAT STOOD HERE IS THE DECLARATION'S, and the claim that stood with it — that it was
           "raised before the options are read" — was FALSE OF THIS SITE while it was true of the rule: a body
           runs after EVERY position is converted, and position 2's dictionary walk is the page's own getters.
           IDL_CALLBACK_INTERFACE_NULLABLE at position 1 is what makes the sentence true. */
        DCHECK(argc == 3, "add/removeEventListener reached its body without all three declared positions — the "
                          "first two are required (§3.6 Overload resolution algorithm step 5, \"If S is empty, "
                          "then throw a TypeError\") and the third is a dictionary the conversion places "
                          "whether or not the page passed one");
        DCHECK(JS_IsNull(argv[1]) || JS_IsObject(argv[1]),
               "§2.7's `EventListener? callback` reached the body as neither the IDL null nor an object — Web "
               "IDL §3.2.16 Callback interface types step 1 is what refuses a primitive, at the declaration, "
               "before §3.6's left-to-right conversion reaches the options bag");
        STEP_GOTO(hdr->stage, AEL_CAPTURE, NULL);
    }

    if (hdr->stage == AEL_CAPTURE) {
        /* The engine-built dictionary, whatever the page wrote: `{capture:true}`, a bare `true`, or nothing.
           §3.2.25's arm was resolved in the declaration — including its fork over an unknown — so there is one
           shape to read here and the bare boolean is already this member. */
        v = idl_dict_get(ctx, opts, "capture");
        rc = ael_flag_run(ctx, hdr, v, AEL_OP_CAPTURE, &s->capture);
        JS_FreeValue(ctx, v);
        if (rc) return rc;
        /* removeEventListener's algorithm is "let capture be the result of flattening options" and then the
           list work: the three members below are declared on AddEventListenerOptions and its own IDL does not
           carry them. */
        STEP_GOTO(hdr->stage, magic == 0 ? AEL_ONCE : AEL_RUN, NULL);
    }

    if (hdr->stage == AEL_ONCE) {
        v = idl_dict_get(ctx, opts, "once");
        rc = ael_flag_run(ctx, hdr, v, AEL_OP_ONCE, &s->once);
        JS_FreeValue(ctx, v);
        if (rc) return rc;
        STEP_GOTO(hdr->stage, AEL_PASSIVE, NULL);
    }

    if (hdr->stage == AEL_PASSIVE) {
        /* STEP 4.2 IS AN `exists` TEST AND NOT A TRUTH TEST. DOM §2.7 Interface EventTarget, flatten more
           options step 4.2: "If options["passive"] exists, then set passive to options["passive"]." AND IT IS
           A TEST ON THE ALREADY-CONVERTED DICTIONARY, WHICH IS WHY THIS STAGE NO LONGER ASKS IT.
           THE THREE WORLDS ARE OBSERVABLY DIFFERENT AND THEY ARE DECIDED ONE SECTION UP. Web IDL §3.2.17
           Dictionary types step 4.1.5 gives an absent member "member's default value", so for
           `boolean capture = false` and `boolean once = false` the absent world IS the false world and two
           arms are complete. AddEventListenerOptions declares `boolean passive;` with NO default, so its
           absent world is null — which DOM §2.7 Interface EventTarget's add an event listener step 4 then
           fills from the default passive value, TRUE for a "touchstart"/"touchmove"/"wheel"/"mousewheel"
           listener on a Window or on its document, document element or body. `{passive:false}` and `{}` are
           then different registrations, which is the difference the flag exists to express.
           OVER AN UNKNOWN OPTIONS BAG THAT WORLD WAS ONCE DELETED HERE, THEN ASKED HERE, AND IS NOW ASKED
           WHERE THE SPEC ASKS IT. `v` was not a datum the page wrote: it was what solver/concolic.c's member
           read MINTS for `Get(unknownBag, "passive")`, with an unconstrained domain containing `undefined`.
           This stage first asserted presence over it (deleting the absent world), then asked a
           three-completion question of its own to get it back. Both were consequences of §3.2.17's member
           loop CROSSING a no-default boolean read off an unknown source, because §3.2.3's two arms cannot say
           `absent`. That loop asks step 4.1.4's presence itself now, so by the time §2.7 runs, `exists` has an
           answer and reading it is a JS_IsUndefined — which is what this stage does. A body-side ask kept
           beside the conversion's would be one spec step decided in two key spaces, and the per-member copy
           of it that every no-default member of every dictionary would then owe. */
        v = idl_dict_get(ctx, opts, "passive");
        if (JS_IsUndefined(v)) {
            s->passive = -1;
            JS_FreeValue(ctx, v);
        } else {
            /* THE PAGE DETERMINED IT AND THE DECLARATION HAS ALREADY CONVERTED IT. Web IDL §3.2.17 Dictionary
               types step 4.1.4 converts a present `boolean` member with ToBoolean, which runs none of the
               page's code, and IDL_BOOLEAN_NO_DEFAULT is where that now happens — so what arrives here is a
               JavaScript boolean and this branch READS it rather than coercing it a second time. It is spelled
               here rather than routed through ael_flag_run because that helper's whole body is the two-arm fork
               this branch has already established is not needed, and a call that can only take the helper's
               first exit would leave a `return rc` no path reaches — an ignore in release wearing a DCHECK in
               dev.
               AND UNKNOWN EXTERNAL INPUT IS NO LONGER A SHAPE THAT REACHES THIS STAGE, WHICH IS WHY THE
               THREE-COMPLETION ASK THAT STOOD HERE IS GONE RATHER THAN NARROWED. It asked absent / present
               -false / present-true over a member the conversion had CROSSED, and it existed because
               §3.2.17's member loop crossed a no-default boolean whose SOURCE was unknown — the loop's two
               §3.2.3 arms could not say `absent`, so the body was left holding the presence question. The
               loop asks step 4.1.4's presence itself now, at the outcome seam, before §3.2.3 is asked: this
               member arrives as a real truth value on the present arm and as `undefined` on the absent one,
               which the branch above already reads as the null flatten more options step 4.2 leaves it at. A
               body-side copy kept beside that would be the same spec step asked twice, in two key spaces,
               with the second one's answer unreachable — so what stands here instead is the assert that says
               the route is closed. */
            DCHECK(JS_IsBool(v),
                   "§2.7's `passive` reached its stage as neither absent nor a converted boolean. Web IDL "
                   "§3.2.17's member loop decides step 4.1.4's PRESENCE at the outcome seam and then converts "
                   "a present member by §3.2.3 at the branch seam, so IDL_BOOLEAN_NO_DEFAULT yields exactly "
                   "those two shapes. UNKNOWN EXTERNAL INPUT here means the loop crosses this member again "
                   "and the presence question is unanswered — the three-armed ask this stage used to make is "
                   "NOT the fix, because it decided in a second key space what the conversion now decides in "
                   "one; anything else is a value that crossed the declaration without being converted at all");
            s->passive = JS_ToBool(ctx, v) ? 1 : 0;
            JS_FreeValue(ctx, v);
        }
        STEP_GOTO(hdr->stage, AEL_SIGNAL, NULL);
    }

    if (hdr->stage == AEL_SIGNAL) {
        /* `AbortSignal signal` IS AN INTERFACE-TYPED MEMBER AND THE BRAND IS ITS DECLARATION'S — see ADD_OPTS,
           which states it as IDL_INTERFACE and whose class core/dom/abort.c owns. The two reasons that stood
           here for performing it by hand are both retired and both were about the DECLARATION rather than
           about this algorithm: that a signal's brand was only its private slot record (it wears the class),
           and that IDL_INTERFACE "cannot express this member's treatment of NULL" (the un-nullable interface
           type is exactly the expression of it).
           NULL IS NOT AN ABSENT MEMBER HERE, AND THE DECLARATION IS WHAT SAYS SO. `AbortSignal signal` is NOT
           nullable, so `{signal: null}` is a TypeError and not "no signal" — Web IDL §3.2.17 Dictionary types
           step 4.1.4 ("If jsMemberValue is not undefined, then: Let idlMemberValue be the result of converting
           jsMemberValue to an IDL value whose type is the type member is declared to be of") sends a present
           null through Web IDL §3.2.15 Interface types, which ends "Throw a TypeError." The corpus asks for
           exactly that, twice, and it asks for it even when the CALLBACK is null — which is why the
           conversion has to happen before §2.7's "if callback is null, return", and being the declaration's is
           what puts it there rather than in a body that runs after every position. Only an ABSENT member
           (undefined) is the null the algorithm means, and the walk crosses one untouched.
           SO THREE SHAPES REACH THIS STAGE AND NOT FIVE: absent, unknown external input (which every declared
           type crosses as itself, so the fork below is untouched by the brand), and a real §3.2 signal. */
        DCHECK(JS_IsUndefined(s->signal),
               "§2.7's `signal` stage was entered holding a signal it had already decided — this stage assigns "
               "the field and the assignment would drop the earlier one without releasing it, so a second entry "
               "with one in place is a stage that leaks whatever the first entry converted");
        v = idl_dict_get(ctx, opts, "signal");
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
        } else if (concolic_is(v)) {
            /* AN UNKNOWN AT THIS POSITION HAS THREE FEASIBLE COMPLETIONS AND USED TO HAVE ONE — a TypeError,
               because a concolic is not a platform object, so `addEventListener('x', f, cfg.opts)` over a bag
               nothing is known about took the whole registration down and every listener behind it. The three
               are what §3.2.15 Interface types and add an event listener step 2 compose to over ONE value, so
               they are asked as ONE question rather than as a brand fork followed by an aborted fork over a
               value the first arm would have had to invent anyway.
               THE NUMBERING IS step_fork_run's ONE RULE. Outcome 0 is the ORDINARY completion — a live signal,
               the listener registers — because a candidate re-fire runs one concrete path and must not be
               diverted on its way to a sink; the two arms that register nothing follow, with the throwing one
               last. */
            int arm = 0;

            rc = step_fork_run(ctx, hdr, v, AEL_OP_SIGNAL, 3, JS_OUTCOME_REAL_UNSTATED, &arm);
            JS_FreeValue(ctx, v);
            if (rc) return rc;
            if (arm == 2) {
                /* THIS IS THE ONLY THROW LEFT IN THIS STAGE AND IT IS THE ENGINE EXPLORING, NOT A PAGE ERROR.
                   The operand is unknown external input, and §3.2.15 Interface types' refusal is one of the
                   three feasible worlds this member has, so the throw is the forced arm doing its job and
                   §Offensive programming names it explicitly as not a `@WHY`. It used to stand beside a
                   byte-identical message for a REAL page type error, and a reader who could not tell them
                   apart read a designed world as an unbuilt capability — which is what happened, from a smoke
                   run, to an expert reader. The page's own TypeError is now §3.2.15's at the member's
                   conversion and never reaches this body, so the two cannot be confused again; what keeps this
                   message honest is that it says which arm it is on, not that it differs from a neighbour.
                   AND SAYING IT IN THE MESSAGE IS NOT SAYING IT TO A CONSUMER. The sentence above was true and
                   was PROSE: the value reaches §8.1.4.6 "Runtime script errors" through the page's own frames,
                   so a reader downstream had a TypeError with a backtrace into the page and one number for two
                   populations — a fixture statement that broke, and a world this engine chose to run. It cost
                   an expert reader a session once and a second reader a second time, from the same line. The
                   declaration below is that same fact as something a consumer can ASK (solver/result.h), made
                   at the only instant it is known and by the site that chose the completion; the throw itself
                   is unchanged, and the page sees byte-identically what it saw before. */
                JS_ThrowTypeError(ctx, "options.signal does not implement AbortSignal (on the forced arm where "
                                       "this flow's unknown `options.signal` is not one — two sibling arms take "
                                       "it as a live signal and as an already-aborted one)");
                result_explored_throw(ctx);
                return -1;
            }
            if (arm == 1)
                return JS_STEP_DONE;   /* add an event listener step 2: an aborted signal registers nothing */
            DCHECK(arm == 0, "§2.7's `signal` question came back on a fourth arm — three completions were "
                             "declared and a fourth is one this member has no algorithm for");
            /* THE ARM IS A WORLD THIS ENGINE BUILDS RATHER THAN SHRUGS AT. On it the value IS a live
               AbortSignal, so step 6 has a signal to add its abort steps to and abort.c's own constructor is
               what makes one — a bare unknown has no §3.2 slot record, and registering an algorithm on it
               would be a step silently skipped. */
            s->signal = abort_signal_new(ctx);
            CHECK(!JS_IsException(s->signal),
                  "the AbortSignal standing for an unknown `options.signal` could not be allocated — a dropped "
                  "one is a listener registered outside the lifetime the page gave it");
        } else {
            /* A REAL §3.2 SIGNAL: there is nothing left to decide about it. Every other known value —
               `{signal: 42}`, `{signal: {}}`, `{signal: null}` — was refused by §3.2.15 Interface types at the
               member's own conversion, which is why the hand-written TypeError that stood here is gone rather
               than kept as a second answer to the section the declaration already runs. */
            DCHECK(abort_signal_is(ctx, v),
                   "§2.7's declared `AbortSignal signal` reached this stage as something that is not an "
                   "AbortSignal — §3.2.15 Interface types' brand is the member's own type and refuses "
                   "everything else BEFORE this point, except unknown external input, which core/idl_args.h "
                   "crosses as itself and the arm above forks. A fourth shape here means the brand ran against "
                   "no class: ADD_OPTS' member carries it and event_target_install is what writes it, so it is "
                   "that write, or core/platform.c's row order under it, that has come apart");
            s->signal = v;
        }
        STEP_GOTO(hdr->stage, AEL_RUN, NULL);
    }

    DCHECK(hdr->stage == AEL_RUN, "add/removeEventListener resumed into a stage §2.7 does not have");
    /* §2.7 "add an event listener" step 3 / "remove an event listener"'s equivalent: a NULL callback registers
       nothing. It is HERE, after every conversion, because that is where the spec puts it — which is what makes
       `addEventListener("x", null, {signal: null})` a TypeError about the signal rather than a silent no-op.
       ONE TEST AND NOT THREE: Web IDL §3.2.20 Nullable types makes null AND undefined the IDL null at the
       declaration, so the value that arrives here IS `null` for both spellings, and the arity is §3.6 step 5's.
       The two extra clauses were the same absent conversion this member's callback position used to have. */
    if (JS_IsNull(argv[1]))
        return JS_STEP_DONE;
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type) return -1;
    r = (magic == 0) ? add_listener_with_type(ctx, event_target_receiver(ctx, hdr->this_val), argv[1], type,
                                              s->capture != 0, s->once != 0, s->passive, s->signal)
                     : remove_listener_with_type(ctx, event_target_receiver(ctx, hdr->this_val), argv[1], type,
                                                 s->capture != 0);
    JS_FreeCString(ctx, type);
    DCHECK(JS_IsUndefined(r),
           "§2.7's listener-list work answered with a value — both members return `undefined` and neither of "
           "these two functions has a path that produces anything else, so a value here is a completion this "
           "machine would have dropped on the floor");
    JS_FreeValue(ctx, r);
    return JS_STEP_DONE;
}

static const IdlStepDecl AEL_DECL = { ael_step, sizeof(AelState), ael_visit, NULL,
                                      "DOM §2.7 Interface EventTarget, addEventListener() and "
                                      "removeEventListener()", AEL_STEPS };

/* ---- EVENT HANDLER IDL ATTRIBUTES — HTML §8.1.8.1 Event handlers --------------------------------------------------------
 *
 * `el.onclick = f` is not a listener registration a page could have written itself with addEventListener; it is
 * its own mechanism, and it was absent entirely. That absence is the single largest entry in this engine's IDL
 * gap report — about 150 of Window's 227 missing members are `on*` attributes, and the same list repeats on
 * Document and Element — and it is absent in the way that hurts most: `window.onload = init` is how a great
 * deal of real code starts, and it silently became an ordinary JS property that nothing ever read.
 *
 * ONE MECHANISM, NOT ONE PER NAME. An event handler attribute is entirely determined by its NAME: `onfoo` is
 * the handler for the event type `foo`. So the names are DATA — one X-macro list, from which the enum, the type
 * strings and the accessor table are all generated — and the behaviour is written once. Spelling the names out
 * rather than generating them from the IDL is the gap engine/idlgen.mjs exists to report; what must not happen
 * is an attribute that answers something its spec does not say.
 *
 * THE HANDLER IS NOT THE LISTENER, and that distinction is the whole design. §8.1.8.1's ACTIVATE AN EVENT
 * HANDLER registers ONE listener the
 * first time a handler is set for a type, and later assignments change the HANDLER the listener reads — so the
 * listener keeps its position in the list. `el.onclick = a; el.addEventListener('click', b); el.onclick = c`
 * runs c then b, not b then c. Registering the handler function itself would append it and get that backwards.
 * So the listener list holds a MARKER for the handler slot, and the list snapshot resolves the marker to
 * whatever the handler is at dispatch time — a read of an engine-built map under a private Symbol, so it runs
 * none of the page's code and needs no request. Setting null removes the marker, which is §8.1.8.1's "deactivate an event handler".
 *
 * The handler map is an own property under a private Symbol, for the reason the listener map is: it makes the
 * handler per-flow for free, so `onclick` assigned in one arm of a fork is invisible to its sibling. */
#define EVENT_HANDLERS(X)                                                                                        \
    /* GlobalEventHandlers — HTML §8.1.8.2 Event handlers on elements, Document objects, and Window              \
       objects, whose first table is the set every HTML element, Document and Window must support. */            \
    X("onabort", "abort", EH_GLOBAL | EH_SIGNAL | EH_XHR | EH_IDB_TRANSACTION | EH_FILE_READER)                                   \
    X("onauxclick", "auxclick", EH_GLOBAL)                                                                       \
    X("onbeforeinput", "beforeinput", EH_GLOBAL)                                                                 \
    X("onbeforematch", "beforematch", EH_GLOBAL)                                                                 \
    X("onbeforetoggle", "beforetoggle", EH_GLOBAL)                                                               \
    /* THE SIX OF §8.1.8.2's SECOND TABLE — the Window-reflecting body element event handler set. That table's   \
       heading is what the second bit says: these are supported "by all HTML elements OTHER THAN body and        \
       frameset elements", and on a body or frameset the same names are "exposed on behalf of that Window        \
       object's associated Document". They are GlobalEventHandlers members in §8.1.8.2.1's IDL like every        \
       other name here, so EH_GLOBAL installs them and EH_WINDOW_REFLECTING adds no member — it is the SET       \
       MEMBERSHIP §8.1.8.1's determine the target of an event handler step 2 tests against. The six are          \
       onblur, onerror, onfocus, onload, onresize and onscroll; onscrollend and onbeforetoggle are NOT among     \
       them, and a prefix or family guess would have taken both. */                                              \
    X("onblur", "blur", EH_GLOBAL | EH_WINDOW_REFLECTING)                                                        \
    /* `oncancel` is a GlobalEventHandlers name AND HTML §6.10.3 The CloseWatcher interface's own event handler \
       IDL attribute, over the SAME event type — one row with two memberships, exactly as `onclose` below is   \
       GlobalEventHandlers' and two other interfaces'. A second X() row would put the name in this list twice, \
       and every consumer of the list would then see a member that does not exist twice over. */               \
    X("oncancel", "cancel", EH_GLOBAL | EH_CLOSE_WATCHER)                                                        \
    X("oncanplay", "canplay", EH_GLOBAL)                                                                         \
    X("oncanplaythrough", "canplaythrough", EH_GLOBAL)                                                           \
    /* `onchange` has THREE owners — GlobalEventHandlers, CSSOM VIEW §4.2's MediaQueryList and Permissions       \
       §6.3's PermissionStatus — which is exactly what the mask is a BITMASK for. A second X() line for the      \
       same name would put the name in this list twice, and every consumer of the list (the IDL auditor, the     \
       content-attribute test) would then see a member that does not exist twice over. */                        \
    X("onchange", "change", EH_GLOBAL | EH_MEDIA_QUERY_LIST | EH_PERMISSION_STATUS)                              \
    X("onclick", "click", EH_GLOBAL)                                                                             \
    /* `onclose` is a GlobalEventHandlers name AND HTML §9.4.4 Message ports' own event handler IDL       \
       attribute AND HTML §6.10.3 The CloseWatcher interface's own, over the SAME event type — one row   \
       with three memberships, exactly as `onmessage` is WindowEventHandlers' and the MessageEventTarget \
       mixin's. A second row would be a second name.                                                     \
    */                                                                                                  \
    X("onclose", "close", EH_GLOBAL | EH_MESSAGE_PORT | EH_CLOSE_WATCHER)                                \
    X("oncommand", "command", EH_GLOBAL)                                                                         \
    X("oncontextlost", "contextlost", EH_GLOBAL)                                                                 \
    X("oncontextmenu", "contextmenu", EH_GLOBAL)                                                                 \
    X("oncontextrestored", "contextrestored", EH_GLOBAL)                                                         \
    /* oncopy, oncut and onpaste were DocumentAndElementEventHandlers and are GlobalEventHandlers members now —  \
       that mixin no longer exists in HTML, and the name appears nowhere in §8.1.8. They keep their place in the \
       spec's alphabetical order rather than sitting under a heading naming a mixin that is gone. */             \
    X("oncopy", "copy", EH_GLOBAL)                                                                               \
    X("oncuechange", "cuechange", EH_GLOBAL)                                                                     \
    X("oncut", "cut", EH_GLOBAL)                                                                                 \
    X("ondblclick", "dblclick", EH_GLOBAL)                                                                       \
    X("ondrag", "drag", EH_GLOBAL)                                                                               \
    X("ondragend", "dragend", EH_GLOBAL)                                                                         \
    X("ondragenter", "dragenter", EH_GLOBAL)                                                                     \
    X("ondragleave", "dragleave", EH_GLOBAL)                                                                     \
    X("ondragover", "dragover", EH_GLOBAL)                                                                       \
    X("ondragstart", "dragstart", EH_GLOBAL)                                                                     \
    X("ondrop", "drop", EH_GLOBAL)                                                                               \
    X("ondurationchange", "durationchange", EH_GLOBAL)                                                           \
    X("onemptied", "emptied", EH_GLOBAL)                                                                         \
    X("onended", "ended", EH_GLOBAL)                                                                             \
    X("onerror", "error", EH_GLOBAL | EH_WINDOW_REFLECTING | EH_XHR | EH_IDB_REQUEST | EH_IDB_TRANSACTION |      \
                          EH_FILE_READER)                                                                        \
    X("onfocus", "focus", EH_GLOBAL | EH_WINDOW_REFLECTING)                                                      \
    X("onformdata", "formdata", EH_GLOBAL)                                                                       \
    X("oninput", "input", EH_GLOBAL)                                                                             \
    X("oninvalid", "invalid", EH_GLOBAL)                                                                         \
    X("onkeydown", "keydown", EH_GLOBAL)                                                                         \
    X("onkeypress", "keypress", EH_GLOBAL)                                                                       \
    X("onkeyup", "keyup", EH_GLOBAL)                                                                             \
    X("onload", "load", EH_GLOBAL | EH_WINDOW_REFLECTING | EH_XHR | EH_FILE_READER)                              \
    X("onloadeddata", "loadeddata", EH_GLOBAL)                                                                   \
    X("onloadedmetadata", "loadedmetadata", EH_GLOBAL)                                                           \
    X("onloadstart", "loadstart", EH_GLOBAL | EH_XHR | EH_FILE_READER)                                                            \
    X("onmousedown", "mousedown", EH_GLOBAL)                                                                     \
    X("onmouseenter", "mouseenter", EH_GLOBAL)                                                                   \
    X("onmouseleave", "mouseleave", EH_GLOBAL)                                                                   \
    X("onmousemove", "mousemove", EH_GLOBAL)                                                                     \
    X("onmouseout", "mouseout", EH_GLOBAL)                                                                       \
    X("onmouseover", "mouseover", EH_GLOBAL)                                                                     \
    X("onmouseup", "mouseup", EH_GLOBAL)                                                                         \
    X("onpaste", "paste", EH_GLOBAL)                                                                             \
    X("onpause", "pause", EH_GLOBAL)                                                                             \
    X("onplay", "play", EH_GLOBAL)                                                                               \
    X("onplaying", "playing", EH_GLOBAL)                                                                         \
    X("onprogress", "progress", EH_GLOBAL | EH_XHR | EH_FILE_READER)                                                              \
    X("onratechange", "ratechange", EH_GLOBAL)                                                                   \
    X("onreset", "reset", EH_GLOBAL)                                                                             \
    /* CSSOM VIEW §12 declares these three on VisualViewport as well, which is what the second bit says. */      \
    X("onresize", "resize", EH_GLOBAL | EH_WINDOW_REFLECTING | EH_VISUAL_VIEWPORT)                               \
    X("onscroll", "scroll", EH_GLOBAL | EH_WINDOW_REFLECTING | EH_VISUAL_VIEWPORT)                               \
    X("onscrollend", "scrollend", EH_GLOBAL | EH_VISUAL_VIEWPORT)                                                \
    X("onsecuritypolicyviolation", "securitypolicyviolation", EH_GLOBAL)                                         \
    X("onseeked", "seeked", EH_GLOBAL)                                                                           \
    X("onseeking", "seeking", EH_GLOBAL)                                                                         \
    X("onselect", "select", EH_GLOBAL)                                                                           \
    X("onslotchange", "slotchange", EH_GLOBAL | EH_SHADOW_ROOT)                                                  \
    X("onstalled", "stalled", EH_GLOBAL)                                                                         \
    X("onsubmit", "submit", EH_GLOBAL)                                                                           \
    X("onsuspend", "suspend", EH_GLOBAL)                                                                         \
    X("ontimeupdate", "timeupdate", EH_GLOBAL)                                                                   \
    X("ontoggle", "toggle", EH_GLOBAL)                                                                           \
    X("onvolumechange", "volumechange", EH_GLOBAL)                                                               \
    X("onwaiting", "waiting", EH_GLOBAL)                                                                         \
    /* THE FOUR HANDLERS WHOSE EVENT TYPE IS NOT THE NAME PAST THE `on` — §8.1.8.2's table gives                 \
       `webkitAnimationEnd`, `webkitAnimationIteration`, `webkitAnimationStart` and `webkitTransitionEnd`, in    \
       camel case, and they are the ONLY four in the whole table that differ from their attribute name. They are \
       why the type is a COLUMN: derived from the name they would have registered a listener for a type nothing  \
       dispatches, and `el.onwebkitanimationend = f` would have been a handler no `webkitAnimationEnd` could     \
       ever reach. The events themselves are CSS Animations' and CSS Transitions'; HTML mandates the attributes  \
       on every HTML element, Document and Window whatever fires them. */                                        \
    X("onwebkitanimationend", "webkitAnimationEnd", EH_GLOBAL)                                                   \
    X("onwebkitanimationiteration", "webkitAnimationIteration", EH_GLOBAL)                                       \
    X("onwebkitanimationstart", "webkitAnimationStart", EH_GLOBAL)                                               \
    X("onwebkittransitionend", "webkitTransitionEnd", EH_GLOBAL)                                                 \
    X("onwheel", "wheel", EH_GLOBAL)                                                                             \
    /* POINTER EVENTS LEVEL 3 §6 Extensions to the `GlobalEventHandlers` mixin — TEN of that section's eleven.   \
       §6's IDL is a `partial interface mixin GlobalEventHandlers`, so these are GlobalEventHandlers members     \
       exactly like §8.1.8.2.1's own, and EH_GLOBAL is the whole of what installs them: every HTML element, every\
       Document and every Window that already gets `onclick` gets these by the same bit and the same accessor.   \
       THEY ARE EVENT HANDLER CONTENT ATTRIBUTES TOO, and that is HTML §8.1.8.1 Event handlers' own statement    \
       rather than this list's inference — "Event handlers are exposed in two ways. The first way, common to all \
       event handlers, is as an event handler IDL attribute. The second way is as an event handler content       \
       attribute. Event handlers on HTML elements and some of the event handlers on Window objects are exposed in\
       this way." Pointer Events itself says nothing about content attributes (the phrase does not occur in it), \
       which is exactly why the answer has to come from HTML: the set is the names of the event handler IDL      \
       attributes, so these ten reach Trusted Types §3.8's TrustedScript test and HTML §8.6.2's remove-unsafe    \
       deny-list through the one list below, and `<img onpointerover=…>` stops being an attribute the Sanitizer  \
       keeps.                                                                                                    \
       NOT COVERED: §6's eleventh row, `[SecureContext] attribute EventHandler onpointerrawupdate`. Every other  \
       row here installs unconditionally and that one must not, so its exposure is a per-REALM question this     \
       installer cannot ask: event_target_install_handlers selects rows by ONE constant mask, and                \
       engine/idl_installed.mjs resolves exactly that one row filter — its `selectorOf` matches a single         \
       `if (!(TBL[i] & param)) continue;` and nothing else — so a second per-row condition would be invisible to \
       it and the name would be credited to every caller in every realm, a false COMPLETE the audit cannot print.\
       THE NEXT DIFF BUILDS a per-realm exposure selector at this installer, answered by                         \
       core/frame/secure_context.h's `secure_context_is`, together with the reading in idl_installed.mjs that    \
       attributes it. ITS ABSENCE SHOWS as `"onpointerrawupdate" in el` answering false on a secure page — an    \
       assignment to it stays an ordinary JS property that no `pointerrawupdate` dispatch reaches — and as       \
       engine/idlgen.mjs reporting it ABSENT on HTMLElement, Document and Window. */                             \
    X("onpointerover", "pointerover", EH_GLOBAL)                                                                 \
    X("onpointerenter", "pointerenter", EH_GLOBAL)                                                               \
    X("onpointerdown", "pointerdown", EH_GLOBAL)                                                                 \
    X("onpointermove", "pointermove", EH_GLOBAL)                                                                 \
    X("onpointerup", "pointerup", EH_GLOBAL)                                                                     \
    X("onpointercancel", "pointercancel", EH_GLOBAL)                                                             \
    X("onpointerout", "pointerout", EH_GLOBAL)                                                                   \
    X("onpointerleave", "pointerleave", EH_GLOBAL)                                                               \
    X("ongotpointercapture", "gotpointercapture", EH_GLOBAL)                                                     \
    X("onlostpointercapture", "lostpointercapture", EH_GLOBAL)                                                   \
    /* THE FOUR OTHER SPECS THAT DECLARE A `partial interface mixin GlobalEventHandlers`, in the shape the       \
       Pointer Events block above already settled: the partial makes each of these a GlobalEventHandlers         \
       member exactly like §8.1.8.2.1's own, so EH_GLOBAL is the whole of what installs them, and the            \
       CONTENT-ATTRIBUTE half is HTML §8.1.8.1 Event handlers' sentence quoted above rather than an inference    \
       of this list's. CSS Animations and CSS Transitions do not even need that sentence — each writes it        \
       itself, in a section whose title is the one HTML uses. CSS ANIMATIONS MODULE LEVEL 1 §5.3 Event           \
       handlers on elements, `Document` objects, and `Window` objects and CSS TRANSITIONS MODULE LEVEL 1 §6.3    \
       Event handlers on elements, `Document` objects, and `Window` objects both read: "The following are the    \
       event handlers (and their corresponding event handler event types) that must be supported by all HTML     \
       elements, as both event handler content attributes and event handler IDL attributes; and that must be     \
       supported by all Document and Window objects, as event handler IDL attributes". Each of those two         \
       sections carries the TABLE the TYPE column below is read from, and neither is a §8.1.8.2 table — these    \
       are other standards' rows, which is why they sit here and not in the alphabetical block above.            \
       THE FOUR LEGACY ALIASES OF THESE VERY EVENTS WERE ALREADY IN THIS LIST, WHICH IS WHY THEIR ABSENCE WAS    \
       A WRONG ANSWER AND NOT MERELY A GAP. `onwebkitanimationend` was here and `onanimationend` was not, so     \
       a bundle's modern handler stayed an ordinary JS property that no `animationend` dispatch could reach      \
       while its 2011 alias was a real handler over the same event — and because                                 \
       event_target_handler_attribute_on_element decides content-attribute membership from this same             \
       EH_GLOBAL bit, `<div ontransitionend="…">` was an attribute HTML §8.6.2's remove-unsafe left in place     \
       and Trusted Types §3.8 never tested, on markup a transition-driven UI writes constantly.                  \
       THE OTHER FOUR ROWS EACH STATE THEIR OWN EVENT TYPE, which is the column that cannot be derived.          \
       SELECTION API's `Extensions to GlobalEventHandlers interface` — cited by TITLE with no number            \
       because that draft prints a section number on nothing — says of each of its two that "The attribute       \
       must be an event handler IDL attribute for the selectstart event supported by all HTML elements" (and     \
       correspondingly for `selectionchange`; the published ED leaves the two operands after that clause as      \
       unprocessed bikeshed markup, so the quotation stops where the rendered text does). WEBXR DOM OVERLAYS     \
       §2.1 onbeforexrselect says "This event is an XRSessionEvent with type beforexrselect that bubbles, is     \
       cancelable, and is composed." FENCED FRAME §3.10.1 onfencedtreeclick event handler says "The table in     \
       the event handlers on elements, Document objects, and Window objects section of [HTML] is modified to     \
       include a new row", and gives that row as `onfencedtreeclick` over `fencedtreeclick` — so it is the       \
       one of these four whose own standard puts it in §8.1.8.2's element table, content-attribute half          \
       included, rather than leaving that to §8.1.8.1's general sentence.                                        \
       NOT COVERED: CSS SCROLL SNAP MODULE LEVEL 2's two, which @webref/idl publishes as `onsnapchanged` and     \
       `onsnapchanging` and which idlgen.mjs therefore charges every one of these interfaces. That draft         \
       DISAGREES WITH ITSELF about both columns: its `IDL Definition` declares those two names, while its own    \
       `Event handlers on elements, Document objects and Window objects` table gives `onscrollsnapchange`        \
       over `scrollsnapchange` and `onscrollsnapchanging` over `scrollsnapchanging`, and the bare types          \
       `snapchanged` and `snapchanging` appear nowhere in the draft as event types at all — every dispatch       \
       and every SnapEvent definition in it is spelled with the `scroll` prefix. A row here needs BOTH           \
       columns, and this list's whole reason for making the type a column is that a guessed one registers a      \
       listener for a type nothing dispatches, which is indistinguishable from a handler nobody set. THE NEXT    \
       DIFF BUILDS these two rows from whichever pair the draft settles on, read from its table and its IDL      \
       block AGREEING; ITS ABSENCE SHOWS as engine/idlgen.mjs reporting `onsnapchanged` and `onsnapchanging`     \
       ABSENT on HTMLElement, Document and Window. `onpointerrawupdate` is likewise still not here, for the      \
       [SecureContext] reason the Pointer Events block above states — a reader completing this family from       \
       the published IDL must not take it as the twelfth of these. */                                            \
    X("onanimationstart", "animationstart", EH_GLOBAL)                                                           \
    X("onanimationiteration", "animationiteration", EH_GLOBAL)                                                   \
    X("onanimationend", "animationend", EH_GLOBAL)                                                               \
    X("onanimationcancel", "animationcancel", EH_GLOBAL)                                                         \
    X("ontransitionrun", "transitionrun", EH_GLOBAL)                                                             \
    X("ontransitionstart", "transitionstart", EH_GLOBAL)                                                         \
    X("ontransitionend", "transitionend", EH_GLOBAL)                                                             \
    X("ontransitioncancel", "transitioncancel", EH_GLOBAL)                                                       \
    X("onselectstart", "selectstart", EH_GLOBAL)                                                                 \
    X("onselectionchange", "selectionchange", EH_GLOBAL)                                                         \
    X("onbeforexrselect", "beforexrselect", EH_GLOBAL)                                                           \
    X("onfencedtreeclick", "fencedtreeclick", EH_GLOBAL)                                                         \
    /* WindowEventHandlers — §8.1.8.2's THIRD table, "reified as event handler IDL attributes through the        \
       WindowEventHandlers interface mixin". It was called the second table here and said the set was            \
       Document's; §8.1.8.2's second table is the six Window-reflecting names above, and the mixin is included   \
       by Window, HTMLBodyElement (§4.3.1) and HTMLFrameSetElement (§16.3.2) and by NO Document — a Document     \
       has only §8.1.8.2's fourth table, `onreadystatechange` and `onvisibilitychange`. These eighteen are       \
       therefore installed on THREE prototypes, and on a body or a frameset every one of them acts upon the      \
       Window per §8.1.8.1's determine the target of an event handler. */                                        \
    X("onafterprint", "afterprint", EH_WINDOW)                                                                   \
    X("onbeforeprint", "beforeprint", EH_WINDOW)                                                                 \
    X("onbeforeunload", "beforeunload", EH_WINDOW)                                                               \
    X("onhashchange", "hashchange", EH_WINDOW)                                                                   \
    X("onlanguagechange", "languagechange", EH_WINDOW)                                                           \
    X("onmessage", "message", EH_WINDOW | EH_PORT)                                                               \
    X("onmessageerror", "messageerror", EH_WINDOW | EH_PORT)                                                     \
    X("onoffline", "offline", EH_WINDOW)                                                                         \
    X("ononline", "online", EH_WINDOW)                                                                           \
    X("onpagehide", "pagehide", EH_WINDOW)                                                                       \
    X("onpagereveal", "pagereveal", EH_WINDOW)                                                                   \
    X("onpageshow", "pageshow", EH_WINDOW)                                                                       \
    X("onpageswap", "pageswap", EH_WINDOW)                                                                       \
    X("onpopstate", "popstate", EH_WINDOW)                                                                       \
    X("onrejectionhandled", "rejectionhandled", EH_WINDOW)                                                       \
    X("onstorage", "storage", EH_WINDOW)                                                                         \
    X("onunhandledrejection", "unhandledrejection", EH_WINDOW)                                                   \
    X("onunload", "unload", EH_WINDOW)                                                                           \
    /* Document's own — §3.1.1 and the Page Visibility API. */                                                   \
    X("onreadystatechange", "readystatechange", EH_DOCUMENT | EH_XHR_READYSTATE)                                 \
    X("onvisibilitychange", "visibilitychange", EH_DOCUMENT)                                                     \
    /* NOT COVERED: FULLSCREEN's `onfullscreenchange` and `onfullscreenerror`, which that standard declares on   \
       BOTH `partial interface Element` and `partial interface Document`. They are the two rows a reader         \
       completing Element's surface from @webref/idl reaches for first, and they are BLOCKED — not small.        \
       WHAT THEY WOULD NEED THAT NO ROW HERE HAS: a bit of their own. Every existing bit is a MIXIN a target     \
       includes or an interface that declares its own set, and these two are declared on Element itself, so      \
       neither EH_GLOBAL (which would make them content attributes on every HTML element — see below) nor        \
       EH_DOCUMENT can carry them. FULLSCREEN §3 API says of both: "The following are the event handlers (and    \
       their corresponding event handler event types) that must be supported by Element and Document objects     \
       as event handler IDL attributes" — IDL ATTRIBUTES AND NOT CONTENT ATTRIBUTES, so the new bit would        \
       have to be one event_target_handler_attribute_on_element answers NO for, exactly as it does for the       \
       two rows above and the twelve below.                                                                     \
       WHY THE BIT IS NOT THE WORK. Nothing in this engine dispatches `fullscreenchange` or `fullscreenerror`,   \
       and this list's standing rule is that a handler attribute for an event no algorithm fires is the          \
       shape-only member the IDL audit exists to expose — the reason IDBDatabase's other three and CSS scroll    \
       snap's two are absent. The events come from FULLSCREEN §2 Model's "run the fullscreen steps", which HTML  \
       §8.1.7.3 Processing model's update the rendering runs at its step 12 — and no component here runs them.   \
       WHAT THE TREE DOES CARRY UNDER THAT NAME is FULLSCREEN §7's `fullscreen` permissions-policy feature,      \
       §2's "fullscreen is supported", its per-element FULLSCREEN FLAG and its FULLSCREEN ELEMENT — read through \
       css-position-4 §3's own ordered accessor, which core/css/top_layer.h exports because that element is the  \
       caller it arrived with — and §3's `fullscreenEnabled`, `fullscreen` and `fullscreenElement` getters, plus \
       HTMLIFrameElement's `allowFullscreen` reflection and the §9.4 step 3 that attribute now performs.         \
       WHAT IT DOES NOT CARRY is anything that SETS the flag. §2's fullscreen an element is its one setter, and  \
       its steps 1 and 2 run HTML §6.12's topmost popover ancestor and hide popovers until. THIS LINE SAID      \
       core/html/popover.c "DFAILs on by name" AND THAT WAS NEVER TRUE OF EITHER — there is no such crash at    \
       any revision: topmost popover ancestor was built and static, and hide popovers until was simply absent  \
       with nothing anywhere naming it, which is the shape a grep for the capability cannot find. Both are     \
       BUILT AND EXPORTED now (core/html/popover.h), so those two steps are two calls; what still does not     \
       exist is the rest of fullscreen an element, so every document's fullscreen element is null, there is no \
       list of pending fullscreen events, and so no `requestFullscreen` and no `exitFullscreen`.               \
       THE NEXT DIFF BUILDS THE FLAG'S WRITER, and the tree already names the join: core/rendering/rendering.c's \
       steps_11_to_13 carries `realm_awaits(docctx, "Document.prototype.exitFullscreen", …)`, a producer probe   \
       that ABORTS the moment that member is installed, naming update-the-rendering step 12 as the thing to      \
       write. So the ordered subproblem is the model and step 12; these two rows are the LAST line of it, not    \
       the first. ADDING THEM ALONE WOULD ALSO BE SILENT: that probe is keyed on `exitFullscreen`, so two        \
       handler rows would install two accessors nothing can ever invoke without tripping anything.               \
       ITS ABSENCE SHOWS as engine/idlgen.mjs reporting `onfullscreenchange` and `onfullscreenerror` ABSENT on   \
       Element, on Document, and on every element interface that inherits them. */                               \
    /* XHR §3.3 — the two of its seven that belong to NO other mixin, so this list is where they arrive. */      \
    X("onloadend", "loadend", EH_XHR | EH_FILE_READER)                                                                            \
    X("ontimeout", "timeout", EH_XHR)                                                                            \
    /* HTML §7.2.6.2's Navigation and §7.2.6.5's NavigationHistoryEntry, each declaring its own. ALL FOUR of     \
       §7.2.6.2's are here now: core/frame/navigate_event_fire.c performs §7.2.6.10.4, which dispatches          \
       `navigate` at the Navigation before every navigation and `navigatesuccess` at the end of one that         \
       committed, and core/frame/navigation_abort.c performs §7.2.6.8's ABORT A NavigateEvent, whose step 6      \
       dispatches `navigateerror` at the same target for every navigation that ends any other way. Each of the   \
       four arrived WITH the algorithm that fires it, because a handler attribute for an event nothing           \
       dispatches is the shape-only member the IDL audit exists to expose. */                                    \
    X("oncurrententrychange", "currententrychange", EH_NAVIGATION)                                               \
    X("onnavigate", "navigate", EH_NAVIGATION)                                                                   \
    X("onnavigateerror", "navigateerror", EH_NAVIGATION)                                                         \
    X("onnavigatesuccess", "navigatesuccess", EH_NAVIGATION)                                                     \
    X("ondispose", "dispose", EH_NAVIGATION_HISTORY_ENTRY)                                                       \
    /* Indexed Database §4.1's `onsuccess` and §4.10's `oncomplete` — the two names that belong to NO other      \
       mixin, so this list is where they arrive. Each came WITH the algorithm that fires it: §5.9's fire a       \
       success event and §5.4's commit task, because a handler attribute for an event nothing dispatches is      \
       the shape-only member the IDL audit exists to expose. */                                                  \
    X("onsuccess", "success", EH_IDB_REQUEST)                                                                    \
    X("oncomplete", "complete", EH_IDB_TRANSACTION)                                                              \
    /* §4.1's IDBOpenDBRequest — "an extended interface to allow listening to the blocked and upgradeneeded      \
       events" — and the one of §4.4's four that something fires. Each arrived WITH its algorithm: §5.1 step     \
       10.5's `blocked`, §5.7 step 10.5's `upgradeneeded` and §5.1 step 10.2's `versionchange`. §4.4's other      \
       three are NOT here: `onclose` needs §5.2's FORCED close, which no user-agent circumstance in this         \
       engine performs, and `onabort`/`onerror` reach a connection only by BUBBLING from a transaction, which    \
       needs §2.7's get-the-parent — until then each would be a handler attribute for an event nothing           \
       dispatches, which is the shape-only member the IDL audit exists to expose. */                             \
    X("onblocked", "blocked", EH_IDB_OPEN_REQUEST)                                                               \
    X("onupgradeneeded", "upgradeneeded", EH_IDB_OPEN_REQUEST)                                                   \
    X("onversionchange", "versionchange", EH_IDB_DATABASE)

/* The NAMES are string literals, not stringified identifiers, so the IDL gap auditor — which scans a component
   for the property names it installs — can SEE them. Behind a `#n` it saw none of these and reported all ninety
   as absent, which is the audit lying by omission: the same failure as leaving an interface out of its map. */
static const char *const EH_NAME[] = {
#define X(n, t, m) n,
    EVENT_HANDLERS(X)
#undef X
};
#define EH_COUNT ((int)(sizeof(EH_NAME) / sizeof(EH_NAME[0])))

/* The EVENT TYPE each attribute handles, WRITTEN OUT rather than derived. It used to be `&(n)[2]` — the name
   past the `on` — and that is not what §8.1.8.2 says: the section gives the type in a TABLE, and for four of
   its rows the table's answer is not the attribute name at all but `webkitAnimationEnd`,
   `webkitAnimationIteration`, `webkitAnimationStart` and `webkitTransitionEnd`, in camel case. A derivation
   that is right ninety-nine times and silently wrong four times is worse than a column: it produced a handler
   registered for a type nothing dispatches, which is indistinguishable from a handler nobody set. */
static const char *const EH_TYPE[] = {
#define X(n, t, m) t,
    EVENT_HANDLERS(X)
#undef X
};

static const int EH_MASK[] = {
#define X(n, t, m) (m),
    EVENT_HANDLERS(X)
#undef X
};

/* THE TWO ACCESSORS OF EVERY ROW, AS ARGS-POOL MEMBERS — one declaration per attribute, because a pool entry
   is what carries the attribute's INDEX (idl_step_magic reads it back off the header) and a member is minted
   once per realm over one entry. Runtime-lifetime like every other step id in this file, indexed by the table
   above, and given back at this component's release.
   THE INITIALISER IS THE X-LIST AND NOT A ZERO FILL, which is core/agent_state.h's pre-init value rather than
   a style: a step id's is -1, a static's default is 0, and 0 is a REAL id — so a zero-filled row would read as
   a declared member in a fresh process and be handed to a mint that would answer some other component's
   machine. Driving the initialiser off EVENT_HANDLERS is also what makes the length agree with EH_COUNT by
   construction, the way the three tables above already do. */
static int g_handler_get_id[] = {
#define X(n, t, m) -1,
    EVENT_HANDLERS(X)
#undef X
};
static int g_handler_set_id[] = {
#define X(n, t, m) -1,
    EVENT_HANDLERS(X)
#undef X
};

/* THE TYPE IS THE NAME PAST THE `on` EXCEPT WHERE §8.1.8.2's TABLE SAYS OTHERWISE, and the exceptions are
   exactly the four legacy webkit aliases. Asserted here rather than trusted, because the two columns are hand
   written and a typo in either is a handler that never fires with nothing to say so. */
static void eh_assert_types(void)
{
    int i, j, reflecting = 0, window = 0;

    for (i = 0; i < EH_COUNT; i++) {
        const char *n = EH_NAME[i], *t = EH_TYPE[i];
        DCHECK(n[0] == 'o' && n[1] == 'n' && n[2] != 0,
               "an event handler IDL attribute was declared with a name that is not `on` plus an event type");
        /* BOTH COLUMNS ARE KEYS, AND NEITHER IS CHECKED BY THE C COMPILER. The NAME column is the property key
           this file interns and defines an accessor pair under, so two rows sharing one name define the same
           property twice and the second install silently wins — a row nobody can reach through its own
           interface. The TYPE column is worse, because it is the key of a MAP: js_handler_set writes the
           handler at `EH_TYPE[magic]` in the target's §8.1.8.1 event handler map and registers the marker
           listener for that same type, so two rows sharing one type share ONE handler slot and ONE listener —
           assigning `el.onA = f` would answer `el.onB` and would fire on B's dispatch. Every mask bit is a
           reason to add another row over the SAME event, which is exactly how a duplicate arrives, and until
           now the two hand-written columns were checked against each other and never against themselves. */
        for (j = 0; j < i; j++) {
            DCHECK(strcmp(EH_NAME[j], n) != 0,
                   "two rows of the event handler list declare the SAME attribute name — the name is the "
                   "property key this file defines an accessor pair under, so the later row's install "
                   "replaces the earlier one and one of the two interfaces answers the other's member; a name "
                   "belonging to several mixins is ONE row carrying several mask bits");
            DCHECK(strcmp(EH_TYPE[j], t) != 0,
                   "two rows of the event handler list name the SAME event type — HTML §8.1.8.1's event "
                   "handler map is keyed by TYPE and so is the marker listener this file registers, so the "
                   "two attributes would share one handler slot: setting either would be readable through "
                   "both and would fire on the other's dispatch");
        }
        DCHECK(strcmp(&n[2], t) == 0 || strncmp(n, "onwebkit", 8) == 0,
               "an event handler's event type is not its name past the `on`, and §8.1.8.2's table names only "
               "the four legacy webkit aliases as exceptions");
        /* §8.1.8.2's second table is a table of GlobalEventHandlers members — §8.1.8.2.1's IDL declares all six
           in that mixin — so a name marked as Window-reflecting and not global is a row this list invented. */
        DCHECK(!(EH_MASK[i] & EH_WINDOW_REFLECTING) || (EH_MASK[i] & EH_GLOBAL),
               "a name was marked as belonging to §8.1.8.2's Window-reflecting body element event handler set "
               "without being a GlobalEventHandlers member — §8.1.8.2.1's IDL declares every one of that "
               "table's six rows in that mixin, so the two bits cannot come apart");
        /* The two halves of §8.1.8.1's determine the target of an event handler step 2 are DISJOINT, and that
           is what makes the union a set rather than an overlap nobody counted: §8.1.8.2's second table is
           supported on Window "as event handler IDL attributes on the Window objects themselves", its third is
           the one reified as WindowEventHandlers. A name in both would be counted twice below. */
        DCHECK(!(EH_MASK[i] & EH_WINDOW_REFLECTING) || !(EH_MASK[i] & EH_WINDOW),
               "a name was marked as both a WindowEventHandlers member and a member of §8.1.8.2's "
               "Window-reflecting body element event handler set — those are two different tables of that "
               "section and no name appears in both");
        if (EH_MASK[i] & EH_WINDOW_REFLECTING) reflecting++;
        if (EH_MASK[i] & EH_WINDOW) window++;
    }
    /* THE TWO SETS §8.1.8.1 STEP 2 TESTS AGAINST, COUNTED. Both are closed lists in §8.1.8.2 — its second table
       has SIX rows (onblur, onerror, onfocus, onload, onresize, onscroll) and §8.1.8.2.1's WindowEventHandlers
       declares EIGHTEEN attributes — and step 2's test is what decides whether `body.onX = f` lands on the
       element or on the Window. Getting the set wrong in EITHER direction is a silent wrong answer rather than
       a gap: one name too few and a bootstrap handler never fires, one too many and a handler a browser puts on
       the body is moved to the Window where nothing dispatching at the body can reach it. */
    DCHECK(reflecting == 6,
           "§8.1.8.2's Window-reflecting body element event handler set is the first column of that section's "
           "SECOND table and that table has six rows — this list marks a different number, so §8.1.8.1's "
           "determine the target of an event handler step 2 is testing against a set the standard does not "
           "define");
    DCHECK(window == 18,
           "§8.1.8.2.1's `interface mixin WindowEventHandlers` declares eighteen attribute members and this "
           "list marks a different number — the mixin is what §8.1.8.1's determine the target of an event "
           "handler step 2 names, and it is also the set a body and a frameset element expose");
}


/* WHICH ATTRIBUTE OF THE LIST ABOVE HANDLES `type`, or -1. §8.1.8.1's processing algorithm takes the NAME of an
   event handler as an argument and the whole of what it uses it for is selecting the Web IDL callback function
   type §8.1.8.2.1 declares — so the walk, which resolves a handler slot by TYPE, has to be able to name the
   attribute it just resolved. The lookup goes through the one X-list rather than by prepending "on", because
   that derivation is exactly the one this file already had to delete: it is right ninety-nine times and wrong
   for the four legacy webkit aliases, where `webkitAnimationEnd` would name `onwebkitAnimationEnd`, which is
   not an attribute at all — and an attribute that is not in the list has no declared IDL type to select. */
static int eh_index_of_type(const char *type)
{
    int i;

    for (i = 0; i < EH_COUNT; i++)
        if (strcmp(EH_TYPE[i], type) == 0)
            return i;
    return -1;
}

/* The handler map (type -> handler) and the marker that stands for it in a listener list. The map is per
   TARGET; the marker is ONE object for the whole runtime, because it carries no information — its identity is
   the whole of what it means. */
static JSValue handler_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-handler map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_handler_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)   /* an own SLOT, never a lookup — see listener_map */
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = idl_slots_new(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* §8.1.8.1's INTERNAL RAW UNCOMPILED HANDLER, MINTED. Its `value` half; its `location` half — "a location where
   the script body originated, in case an error needs to be reported" — is the one thing this record does not
   carry, and it is ABSENT rather than defaulted: the location §8.1.8.1's attribute change steps step 5.4 asks
   for is "the script location that TRIGGERED the execution of these steps", which is the chunk and line of
   whatever ran `el.setAttribute("onclick", …)` and is the document's own address only for the parser's writes.
   Deriving it from the element's document would be right for one of those two and a plausible datum for the
   other, and its ONLY consumer is §8.1.8.1 step 3.7.2's SyntaxError, which that step says "should be based
   on location, where the script body originated".
     THE COMPILE IS NO LONGER WHAT WAITS ON IT, and this used to say it was. RETIRED TEXT, unquoted because
   it is this file's and not a standard's: its ONLY consumer is a syntax error the compile below does not
   exist to report yet — the crash in handler_current names it beside the compile, so the diff that reads it
   is the diff that writes it. Step 3.7 is BUILT and the two have come
   apart. handler_current now decides step 3.7's condition and takes both of its arms, and the location is owed
   by the UNPARSABLE arm alone, whose residual names it; a parsable body never reads this field at all. So the
   diff that writes it is the diff that performs steps 3.7.2-3.7.3, which is not the compile.
   Returns JS_EXCEPTION on allocation failure, like every other constructor here. */
static JSValue uncompiled_new(JSContext *ctx, const char *body, size_t body_n)
{
    JSValue rec, b;
    JSAtom k;

    DCHECK(g_ready, "an internal raw uncompiled handler was minted before its brand existed");
    DCHECK(body != NULL, "an internal raw uncompiled handler was minted with no uncompiled script body — "
                         "§8.1.8.1's tuple has one, and a removed attribute is step 4's DEACTIVATE rather "
                         "than a handler whose body is nothing");
    rec = idl_slots_new(ctx);
    if (JS_IsException(rec))
        return rec;
    b = JS_NewStringLen(ctx, body, body_n);
    if (JS_IsException(b)) { JS_FreeValue(ctx, rec); return JS_EXCEPTION; }
    k = JS_ValueToAtom(ctx, g_uncompiled_key);
    if (k == JS_ATOM_NULL) { JS_FreeValue(ctx, rec); JS_FreeValue(ctx, b); return JS_EXCEPTION; }
    JS_SetProperty(ctx, rec, k, b);
    JS_FreeAtom(ctx, k);
    return rec;
}

/* …AND RECOGNISED: the uncompiled script body of `h`, or JS_UNDEFINED when `h` is not one of these records.
   AN OWN SLOT AND NEVER A LOOKUP, for handler_map's reason — the brand is a private Symbol the page cannot
   name, so nothing it can build carries one, and asking about STORAGE rather than about a get keeps that true
   for an object with a Proxy in its prototype chain. Owned. */
static JSValue uncompiled_body(JSContext *ctx, JSValueConst h)
{
    JSValue b;
    JSAtom k;

    if (!JS_IsObject(h))
        return JS_UNDEFINED;
    DCHECK(g_ready, "an event handler's value was tested for §8.1.8.1's uncompiled brand before it existed");
    k = JS_ValueToAtom(ctx, g_uncompiled_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &b, h, k) <= 0)
        b = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsUndefined(b) || JS_IsString(b),
           "§8.1.8.1's internal raw uncompiled handler brand held something that is not the uncompiled script "
           "body — the brand and the body are ONE slot precisely so a record carrying the first without the "
           "second cannot exist, so this is a second writer of a key only uncompiled_new may write");
    return b;
}

/* THE MEMBERS OF §8.1.8.1 STEP 3.9's SCOPE — one bit per substep that ADDS a NewObjectEnvironment, and the bit
   number is the carrier slot the source text reads. Substeps 1 and 2 (the realm, and realm.[[GlobalEnv]]) add
   no layer and have no bit: globalEnv is what a JS_EVAL_TYPE_GLOBAL program already has, and substep 6 returns
   the scope. Each condition is the standard's own and is decided at the compile, never here:
     EH_SCOPE_DOCUMENT  substep 3, "If eventHandler is an element's event handler". Its own note is what makes
                        that the same question as step 3.1's partition: "(Otherwise, eventHandler is a Window
                        object's event handler.)" — there is no third kind of handler for the algorithm to be
                        about, so the element arm of step 3.1 IS this condition.
     EH_SCOPE_FORM      substep 4, "If form owner is not null" — step 3.5's form owner, which is null for an
                        element that has none and for every Window.
     EH_SCOPE_ELEMENT   substep 5, "If element is not null" — step 3.1's element. */
#define EH_SCOPE_DOCUMENT (1u << 0)
#define EH_SCOPE_FORM     (1u << 1)
#define EH_SCOPE_ELEMENT  (1u << 2)

/* §8.1.8.1 step 3.9's SOURCE TEXT, PARENTHESISED — the string that step concatenates out of the name, the
   parameter list, a U+000A LF, the body, a U+000A LF and a closing brace, with step 3.9's own parameterList
   spelled into it: "Let the function have a single argument called event", and for `onerror` on a Window
   "Let the function have five arguments, named event, source, lineno, colno, and error". Owned by the caller
   — `free()` it; `*plen` is its byte length, which is what the parser is given, so a body holding a U+0000 is
   still the body it holds.
     THE TWO LINE FEEDS ARE THE STANDARD'S AND THE TRAILING ONE IS LOAD-BEARING: a body ending in a `//`
   comment would otherwise comment out the closing brace, which is the same defence §20.2.1.1.1
   CreateDynamicFunction step 14 states for its own bodyParseString.
     THE FUNCTION EXPRESSION IS ANONYMOUS AND THE STANDARD'S SOURCE TEXT IS NAMED, which is a deliberate
   divergence and not a shortcut. Step 3.9 calls §10.2.3 OrdinaryFunctionCreate ( proto, sourceText, paramList,
   body, thisMode, envRecord, privateEnv ), which creates NO binding for the name — the name lives only in
   sourceText — while a named function EXPRESSION binds its own name in a declarative environment inside the
   innermost scope layer, so `<button onclick="onclick = null">` would assign to an immutable function-name
   binding instead of to the element. The residual is [[SourceText]]: `String(el.onclick)` omits the name.
     THE PARENTHESES ARE THIS ENGINE'S, for js_parse_fn_ctor_source's reason — what this engine needs is the
   closure the expression evaluates to, and a parenthesised FunctionExpression pinned to end of input admits
   exactly the texts the bare FunctionExpression goal admits.
     `layers` IS STEP 3.9's SCOPE, SPELLED RATHER THAN SIMULATED, and it is a set over substeps 3, 4 and 5 —
   EH_SCOPE_DOCUMENT, EH_SCOPE_FORM, EH_SCOPE_ELEMENT — each of which is one NewObjectEnvironment. ECMAScript
   §14.11.2 Runtime Semantics: Evaluation step 4 is "Let newEnv be NewObjectEnvironment(obj, true, oldEnv)":
   the SAME abstract operation with the SAME isWithEnv that §9.1.2.3 NewObjectEnvironment ( obj, isWithEnv,
   outerEnv ) takes, so a `with` head IS the layer and not a stand-in for one. Empty gives the pinned bare
   expression above, which is the Window case; a non-empty set gives

       with (this[0]) with (this[1]) with (this[2]) (function (event) {\n<body>\n});

   whose OUTERMOST head is substep 3's and whose innermost is substep 5's, because each substep re-assigns
   `scope` with the previous one as its outer environment. The heads are emitted only for the members of the
   set, and the SLOT NUMBER stays the substep's whatever the set is: an absent form owner drops the head, never
   renumbers the two that remain. An ordinal is admissible here for the one reason §CLAUDE.md allows it — the
   set is the STANDARD's, fixed at three by the text, and it is the carrier's own indices rather than a
   position in anything a page can mutate.
     `this` IS THE ONE CHANNEL A LAYER CANNOT SHADOW, which is what makes the heads readable at all. A named
   temporary would be resolved through the object environments the earlier heads just installed, so a page with
   `<form name=x>` could re-point the second head; `this` is a PSEUDO VARIABLE in quickjs's own resolver
   (resolve_scope_var's `is_pseudo_var` names JS_ATOM_this and the `__with_` arm is guarded by !is_pseudo_var),
   so no `with` layer is consulted for it, and `this[0]` is then an ordinary property read on an object no page
   holds. The carrier is built with a NULL prototype for the second half of that: an inherited slot would be a
   scope layer the standard does not name.
     THE PROGRAM'S VALUE IS STILL THE FUNCTION EXPRESSION with the heads present, and that is quickjs's own
   emission rather than an inference: the `with` statement emits set_eval_ret_undefined before its body and the
   ExpressionStatement arm writes the program's hidden completion slot, which is §14.11.2 step 8's
   UpdateEmpty(stmtCompletion, undefined) — a non-empty inner completion passes straight out. So there is no
   wrapper call to make and no function environment between the innermost layer and the handler, which there
   would be if the heads sat inside a synthesized function.
     THE HEADED SHAPE CANNOT CARRY THE END-OF-INPUT PIN, and what makes it sound instead is STEP 3.7.
   js_parse_fn_ctor_source's first act is js_parse_expect(s, '(') under a DCHECK that the next token is
   `function`, so a source beginning `with` is refused by construction and no flag changes that. It does not
   need it: a body that could close the wrapper — `}); alert(1); ({` — is not a FunctionBody ALONE, so step
   3.7 answered NOT PARSABLE and handler_current returned null instead of the record. Every body that reaches
   the compile has been through that pinned probe, over the identical `(function (event) {` prefix and the
   identical following token, so its token stream here is the one the pin accepted and it is balanced. The
   compile asserts that rather than assuming it. */
static char *handler_source(JSContext *ctx, JSValueConst body, bool five, unsigned layers, size_t *plen)
{
    static const char PRE1[] = "(function (event) {\n";
    static const char PRE5[] = "(function (event, source, lineno, colno, error) {\n";
    static const char POST[] = "\n})";
    static const char *const HEAD[3] = { "with (this[0]) ", "with (this[1]) ", "with (this[2]) " };
    const char *pre = five ? PRE5 : PRE1;
    size_t npre = (five ? sizeof PRE5 : sizeof PRE1) - 1;
    size_t nbody = 0, nhead = 0, at = 0;
    const char *b = JS_ToCStringLen(ctx, &nbody, body);
    /* The ExpressionStatement's semicolon, present only where there are heads — see the banner. */
    size_t ntail = (sizeof POST - 1) + (layers != 0 ? 1u : 0u);
    char *out;
    int k;

    /* THE BODY IS A STRING THIS FILE MINTED (uncompiled_new writes it and nothing else may), so a failure here
       is an allocation failure and not a coercion the page can steer — which is why it is a CHECK. */
    CHECK(b != NULL, "the uncompiled script body could not be read as bytes — it is a String uncompiled_new "
                     "minted, so this is an allocation failure");
    DCHECK((layers & ~7u) == 0,
           "§8.1.8.1 step 3.9's scope was asked for a layer that is not one of substeps 3, 4 and 5 — the "
           "argument is a set over exactly those three and the carrier this source reads has three slots");
    for (k = 0; k < 3; k++)
        if (layers & (1u << k))
            nhead += strlen(HEAD[k]);
    out = malloc(nhead + npre + nbody + ntail + 1);
    CHECK(out != NULL, "§8.1.8.1 step 3.9's source text could not be allocated");
    for (k = 0; k < 3; k++)
        if (layers & (1u << k)) {
            memcpy(out + at, HEAD[k], strlen(HEAD[k]));
            at += strlen(HEAD[k]);
        }
    DCHECK(at == nhead, "§8.1.8.1 step 3.9's scope heads were measured and written at different lengths");
    memcpy(out + at, pre, npre);
    memcpy(out + at + npre, b, nbody);
    memcpy(out + at + npre + nbody, POST, sizeof POST - 1);
    at += npre + nbody + sizeof POST - 1;
    if (layers != 0)
        out[at++] = ';';
    out[at] = '\0';   /* the parser never reads it; the length below is what it is given */
    JS_FreeCString(ctx, b);
    *plen = at;
    return out;
}

/* §8.1.8.1 step 3.7's CONDITION — "if body is not parsable as FunctionBody or if parsing detects an early
   error" — answered the other way round: true when the body IS parsable.
     IT IS ASKED OF THE BODY WITH ITS PARAMETER LIST AND NEVER OF THE BODY ALONE, because the step's second
   disjunct is about EARLY ERRORS and a function's early errors are stated over its parameters and its body
   together. `<img onerror="let event;">` is a SyntaxError in every browser precisely because `event` is step
   3.9's parameterList; a probe with an empty parameter list would accept it here and the compile would then
   fail — a rejection arriving one step late, in the wrong algorithm, with no step of the standard to report it.
     THE PIN IS js_parse_fn_ctor_source's END OF INPUT, and it is what makes this EXACT rather than close. A
   Script's body is a StatementList, so a PROGRAM parse of the same text accepts a body that CLOSES the
   wrapper: `}); alert(1); ({` is three source elements, every one of which a program may hold, and the engine
   then runs the page's `alert(1)` at the point the standard makes a parse. Pinned to end of input as one
   parenthesised FunctionExpression, a body that closes the wrapper has nowhere to put what follows.
     JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_FUNCTION_CTOR IS THAT PIN AND DECIDES NOTHING ELSE, which is a fact
   about the flags rather than a convention borrowed from `new Function`. That flag has exactly two consumers
   in quickjs.c: `s->fn_ctor_toplevel`, which routes the top-level parse to js_parse_fn_ctor_source, and
   `fd->from_eval`, which is `(eval_type == DIRECT || INDIRECT) && !FUNCTION_CTOR` and is therefore ALREADY
   false at JS_EVAL_TYPE_GLOBAL. So at this eval type the flag selects the pinned parse and says nothing about
   eval origin — which is also the right answer, since an event handler is not eval code.
     THE EVAL TYPE IS ALSO WHAT KEEPS THIS OFF THE @S SEAM. §19.2.1.2 HostEnsureCanCompileStrings is performed
   by §19.2.1.1 PerformEval step 5 and §20.2.1.1.1 CreateDynamicFunction step 11 and by nothing else, so the
   sink is named by DIRECT ∪ INDIRECT; a GLOBAL compile announces nothing, and announcing this one would report
   every inline handler on every page as a code-execution sink — a fabricated finding, which propagates.
     THE PENDING SyntaxError IS CONSUMED HERE, and this is the one place in this file where consuming one is
   correct rather than a swallow: the parse failure IS this step's condition. Step 3.7.2 mints its OWN
   SyntaxError, one that "describes the error while parsing" and is based on the handler's location, so the
   probe's exception is not that exception and must not escape into an algorithm that has not reached its
   throw.
     RUNS NO PAGE CODE. JS_EVAL_FLAG_COMPILE_ONLY produces bytecode and evaluates nothing, so step 3.7 needs no
   rest point and no flow base — which is why it can be performed from the dispatch walk and from the plain C
   accessor alike, and why it is the half of step 3 that lands first. */
static bool handler_body_parsable(JSContext *ctx, JSValueConst body, bool five)
{
    size_t n = 0;
    /* NO SCOPE LAYERS, AND THAT IS THE STEP'S OWN SHAPE: 3.7 asks whether the body "is not parsable as
       FunctionBody", which is a question about the body and its parameter list ALONE. The layers are step
       3.9's, they are a SCOPE rather than a grammar, and a probe carrying them would be asking a different
       question — and could not be pinned, since a source beginning with a `with` head is one
       js_parse_fn_ctor_source refuses by construction. */
    char *src = handler_source(ctx, body, five, 0, &n);
    JSValue bc = JS_Eval(ctx, src, n, "<event handler>",
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_FUNCTION_CTOR | JS_EVAL_FLAG_COMPILE_ONLY);

    free(src);
    if (JS_IsException(bc)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    DCHECK(JS_VALUE_GET_TAG(bc) == JS_TAG_FUNCTION_BYTECODE,
           "§8.1.8.1 step 3.7's probe compiled without an exception and did not answer with a program — "
           "JS_EVAL_FLAG_COMPILE_ONLY's whole contract is that it produces bytecode and evaluates nothing, so "
           "anything else here means this compile RAN, which for a page's inline handler is the escape the "
           "end-of-input pin exists to make impossible");
    JS_FreeValue(ctx, bc);
    return true;
}

/* §8.1.8.1's GETTING THE CURRENT VALUE of the event handler for `type` on `target` — ITS CODE-FREE HALF.
   Answers one of THREE things, and the third is what makes this function's contract worth reading: JS_NULL;
   the page's own assigned value; or, when step 3 applies and step 3.7 says the body parses, §8.1.8.1's
   INTERNAL RAW UNCOMPILED HANDLER RECORD ITSELF, meaning steps 3.8-3.12 are owed and the caller must perform
   them through handler_compile_run. `uncompiled_body` is the test for that third case and a caller that
   cannot compile must CRASH on it rather than pass it on — a record is not a callback.
   NO PAGE CODE
   and no request — which is what lets the dispatch walk resolve the marker in place. RETIRED TEXT, unquoted because it is this
   file's and not a standard's: this used to justify itself as a map read, so no page code. The premise has moved while the conclusion has not: step 3.7 makes this a map
   read AND A PARSE, and a parse of the page's own markup is still none of the page's code, because
   JS_EVAL_FLAG_COMPILE_ONLY evaluates nothing. The day step 3.7.2's report lands, the conclusion goes too —
   reporting an exception FIRES AN EVENT, and firing one runs the page's listeners.
   AN OBJECT, NOT A FUNCTION. `EventHandler` is `EventHandlerNonNull?` and Web IDL §3.2.19 Callback function
   types says a callback function type IS a function object "except in the [LegacyTreatNonObjectAsNull] case,
   when they can be ANY object" — so `el.onclick = {}` stores that object, `el.onclick` gives it back, and the
   TypeError arrives at §3.12's invoke when the event fires. Filtering to callables here answered `null` for a
   value the page had assigned and could read back in every browser. */
static JSValue handler_current(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue map = handler_map(ctx, target, 0), h, body;

    /* STEPS 1-2 — "let handlerMap be eventTarget's event handler map", "let eventHandler be handlerMap[name]".
       A target with no map has never had a handler set, which is every entry's initial null value. */
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_NULL; }
    h = JS_GetPropertyStr(ctx, map, type);
    /* STEP 3 — "if eventHandler's value is an INTERNAL RAW UNCOMPILED HANDLER". Its twelve substeps are the
       COMPILE, and they are the whole of what turns `<button onclick="doThing()">` into something that runs. */
    body = uncompiled_body(ctx, h);
    if (!JS_IsUndefined(body)) {
        int i = eh_index_of_type(type);
        /* STEP 3.9's parameterList, DECIDED HERE BECAUSE STEP 3.7 NEEDS IT. The two steps look independent —
           one tests the body, the other builds the function — and they are not: a function's early errors are
           stated over its parameters and its body together, so the question "is this body parsable" has no
           answer until the parameter list is known. The condition is step 3.9's own, verbatim: five arguments
           "if name is onerror and eventTarget is a Window object", one otherwise. */
        bool five = (i >= 0 && strcmp(EH_NAME[i], "onerror") == 0 && event_target_is_window(ctx, target));
        bool parsable = handler_body_parsable(ctx, body, five);

        JS_FreeValue(ctx, body);
        if (!parsable) {
            /* STEP 3.7.1 — "Set eventHandler's value to null" — and its own note, which is why this is a bare
               map write and not a call to handler_deactivate: "This does not deactivate the event handler,
               which additionally removes the event handler's listener (if present)." The MARKER stays in the
               listener list holding its position; only the value goes. The next dispatch reads null, step 3
               does not apply, and §2.9's walk skips a slot that resolves to null — which is what a browser
               does with markup that would not parse, and what makes this a state and not a repeated failure.
               STEP 3.7.4 — "Return null" — is the return below.
               RESIDUAL: STEPS 3.7.2 AND 3.7.3 ARE NOT PERFORMED. What is above is CORRECT for what it does and
               NARROWER than the step. WHAT IS NOT COVERED: the SyntaxError step 3.7.2 mints, and HTML
               §8.1.8.1 step 3.7.3's "Report an exception with syntaxError for settings object's global
               object". WHAT THE NEXT DIFF
               BUILDS: (i) the LOCATION on the uncompiled record — uncompiled_new does not carry it and says
               why, and §8.1.8.1's attribute change steps step 5.4 is where it comes from — and (ii) the report
               itself through report_exception_run, which this file's own dispatch machine already drives with
               a ReportExceptionWork of its own; that sub-sequence PARKS, so performing it here requires
               handler_current to be reached from a machine rather than from plain C. THAT USED TO SAY the
               compile needed the same rest point "and is why the two land together", and the compile has
               landed WITHOUT it: steps 3.8-3.12 are a sub-algorithm the CALLER drives (handler_compile_run),
               so this function stayed plain C and the report did not come with it. What the report still
               needs is its own such sub-algorithm, driven the same way. HOW ITS ABSENCE SHOWS:
               `<img src=x onerror="}">` fires no `error` event at the Window and puts nothing in the
               page-error stream, where a browser reports a SyntaxError at the attribute's position. It does
               NOT show in the getter — `el.onerror` is null either way — so the getter is the wrong place to
               look for it. */
            JS_SetPropertyStr(ctx, map, type, JS_NULL);
            JS_FreeValue(ctx, map);
            JS_FreeValue(ctx, h);
            return JS_NULL;
        }
        JS_FreeValue(ctx, map);
        /* `h` IS NOT FREED HERE — it is the RETURN VALUE now, and its reference transfers to the caller. The
           arm above frees it because that one answers null; this one answers the record itself. */
        /* THE BODY PARSES, SO STEP 3 IS NOT DONE — IT IS OWED, AND THIS RETURNS THE RECORD SO THE CALLER
           CAN FINISH IT. Steps 3.8-3.12 are the compile, and they CALL a bytecode body (see the sub-algorithm
           below), which is a thing no plain C frame may do: quickjs.c's JS_CallInternal DFAILs unconditionally
           on a bytecode body entered by C recursion below a live flow, whatever that body contains. So this
           function stops where the code-free half stops, and hands its caller §8.1.8.1's own internal raw
           uncompiled handler back.
           THE RECORD IS A LEGITIMATE RETURN AND NOT A SENTINEL, which is why nothing here has to encode
           "owed" a second way: it is exactly `eventHandler`'s value at the point step 3 is entered, it carries
           its own private-Symbol brand, and uncompiled_body is the test. A caller that can compile calls
           handler_compile_run; a caller that cannot CRASHES rather than handing it on, because the record is
           not a callback and a page must never see one.
           WHAT IS PERFORMED ABOVE IS STEP 3.7 — its condition, and its arms 3.7.1 and 3.7.4 — and step 3.1 is
           answered by the caller, which hands the target this walk resolved. NOT step 3.2, and this sentence
           is deliberately a list of steps rather than a range, because a range is the shape that reads as
           covered:
             STEP 3.2 — "If document's active sandboxing flag set has its sandboxed scripts browsing context
                 flag set, then return null." The engine HAS that set (core/frame/sandboxing.h's
                 SANDBOX_SCRIPTS, core/dom/document.h's document_active_sandbox_flags). THIS USED TO SAY the
                 missing piece was WHOSE document — that step 3.1's document is the ELEMENT's node document and
                 nothing here could name it — and step 3.9's scope retired HALF of that argument: this
                 section's defined terms now answer an element's node document, so the element arm of step 3.1
                 is nameable. What is still missing is the OTHER arm, "eventTarget's associated Document" for a
                 Window, and step 3.2 is asked BEFORE the partition is used for anything else, so half of it is
                 not a step — one arm answered and one guessed is the plausible datum the old sentence was
                 about, moved one arm over. HOW ITS ABSENCE SHOWS: an inline handler inside `<iframe sandbox>`
                 (no `allow-scripts`) compiles and runs here, where a browser answers null.
             STEP 3.5 — the form owner — IS PERFORMED, and not here: it is an input to step 3.9's scope substep
                 4 and is taken at handler_compile_run, on the arm that has an element to ask about.
             STEP 3.6 — the settings object — is an input to step 3.7.2's report, and is named where that is
                 owed, at the unparsable arm above. */
        (void)i;
        return h;   /* the RECORD, for the caller to compile — see the contract on this function */
    }
    JS_FreeValue(ctx, map);
    return JS_IsObject(h) ? h : (JS_FreeValue(ctx, h), JS_NULL);
}

/* IS §8.1.8.1 STEP 3'S COMPILE OWED ON THIS VALUE — the third answer handler_current can give, asked as a
   predicate so no call site has to know that the record's brand and its body are one slot. */
static bool handler_compile_owed(JSContext *ctx, JSValueConst h)
{
    JSValue body = uncompiled_body(ctx, h);
    bool owed = !JS_IsUndefined(body);

    JS_FreeValue(ctx, body);
    return owed;
}

/* HTML §8.1.8.1 "Event handlers"' GET THE CURRENT VALUE OF THE EVENT HANDLER, STEPS 3.8-3.12 — the compile
 * that turns an internal raw uncompiled handler into something a dispatch can invoke. §@S's "ONLY FIRING
 * proves it" is a sentence about this function: without it every `onerror=` and `onload=` a page writes in
 * markup is registered, positioned in the listener list, and never called.
 *
 * IT IS A SUB-ALGORITHM OF A STEP MACHINE AND NOT A MACHINE, which is event_handler.h's shape and is taken
 * from it deliberately: the one caller that can reach this is already a machine (DOM §2.9's dispatch walk), so
 * the record is the CALLER's — one stage byte it owns — and the call request borrows the caller's `cphase` and
 * `cb` buffer rather than growing a second copy of both. `stage` is ZERO exactly when no compile is in flight,
 * which is what the caller's resume routing reads.
 *
 * WHY IT MUST PARK AT ALL, given that it runs none of the page's code. Step 3.9's function is produced by
 * EVALUATING a program (see below), and quickjs.c's JS_CallInternal DFAILs on a bytecode body entered by C
 * recursion below a live flow UNCONDITIONALLY — it does not ask what the body contains, and it is right not
 * to, because a rule that exempted bodies believed to be trivial is a rule whose next reader widens it. So the
 * call is a REQUEST like every other, through step_call_run.
 *
 * STEP 3.9's SCOPE IS SIX SUBSTEPS AND EACH LAYER IS TAKEN ON ITS OWN CONDITION, never on a summary of them.
 * Substep 1 is the realm and substep 2 is realm.[[GlobalEnv]] — what a JS_EVAL_TYPE_GLOBAL program already
 * has, so those two are the compile below and nothing more. Substeps 3, 4 and 5 each add ONE
 * NewObjectEnvironment, for the DOCUMENT "if eventHandler is an element's event handler", the FORM OWNER "if
 * form owner is not null" and the ELEMENT "if element is not null", innermost last; substep 6 returns the
 * scope. Step 3.1 partitions the target into an ELEMENT or a Window, and that partition decides all three:
 *   A WINDOW's HANDLER takes none of them — element is null, form owner is null, and substep 3's own note
 *     ("Otherwise, eventHandler is a Window object's event handler.") is what makes its condition false — so
 *     the scope is realm.[[GlobalEnv]] and nothing else, exactly the scope of a plain global program.
 *     `<body onload="…">` is that case and not an exotic one: determine the target of an event handler moves a
 *     body element's WindowEventHandlers attributes onto the Window, so the commonest inline handler on the
 *     web is a Window's.
 *   AN ELEMENT's HANDLER takes substeps 3 and 5 unconditionally on this arm and substep 4 only where step
 *     3.5's form owner is non-null, so `<img onerror>` gets two layers and `<input onchange>` inside a form
 *     gets three. THE LAYERS ARE NOT OPTIONAL AND FLATTENING THEM IS NOT A NARROWING: `<body onclick="write(
 *     'x')">` is an ELEMENT's handler — `onclick` is in NEITHER set determine the target of an event handler
 *     step 2 tests against, neither §8.1.8.2's six-row Window-reflecting body element event handler set nor
 *     §8.1.8.2.1's WindowEventHandlers mixin — and it resolves `write` on the DOCUMENT layer, where with
 *     globalEnv alone the same source silently assigns a global and nothing downstream reports it. Each layer
 *     is a `with` head; see handler_source's banner for why that IS the abstract operation and not a stand-in
 *     for it, and for what makes the headed shape sound without the end-of-input pin.
 *
 * RESIDUAL: STEP 3.11 IS NOT PERFORMED. WHAT IS NOT COVERED: "Set function.[[ScriptOrModule]] to null". WHAT
 * THE NEXT DIFF BUILDS: a per-function script-or-module slot to null out — this engine has none, deriving the
 * name from the STACK (JS_GetScriptOrModuleName walks n_stack_levels), so there is no field here to write and
 * the step is not skipped so much as unreachable. HOW ITS ABSENCE SHOWS: a dynamic `import()` inside an inline
 * handler resolves a relative specifier against the nearest script on the stack instead of falling back to the
 * current settings object's API base URL, which is the one effect the standard's own note says this step has.
 *
 *   > 0  — parked; the caller returns the code as it stands.
 *   0    — the compile is complete, `*pout` is the handler function (owned) and step 3.12 has written it to
 *          the handler map, so the NEXT dispatch reads a function and never reaches this again.
 *   -1   — the program evaluation was abrupt. */
static int handler_compile_run(JSContext *ctx, uint8_t *stage, uint8_t *cphase, JSValue *cb, int cb_cap,
                               JSValueConst target, const char *type, JSValue in, JSValue *pout,
                               JSValue **out_cb, int *out_argc)
{
    JSValue fn = JS_UNDEFINED;
    int r;

    if (*stage == 0) {
        JSValue map = handler_map(ctx, target, 0), h, body, prog, recv;
        JSValue carrier = JS_UNDEFINED;
        unsigned layers = 0;
        size_t n = 0;
        char *src;
        int i = eh_index_of_type(type);
        bool five, is_win;

        DCHECK(JS_IsObject(map),
               "§8.1.8.1 step 3's compile was entered for a target with no event handler map — the record it "
               "is about was read out of that map one step earlier, so the map cannot have gone");
        h = JS_GetPropertyStr(ctx, map, type);
        body = uncompiled_body(ctx, h);
        DCHECK(!JS_IsUndefined(body),
               "§8.1.8.1 step 3's compile was entered for a handler whose value is not an internal raw "
               "uncompiled handler — handler_current answers the RECORD exactly when steps 3.8-3.12 are owed, "
               "so this is a caller that decided the compile was owed some other way");

        /* STEP 3.1's PARTITION — "If eventTarget is an element, then let element be eventTarget, and document
           be element's node document. Otherwise, eventTarget is a Window object, let element be null, and
           document be eventTarget's associated Document." It is asked ONCE and decides two different things
           below, which is why it is a local and not two calls: step 3.9's parameterList, and which of step
           3.9's scope substeps have anything to add. */
        is_win = event_target_is_window(ctx, target);
        /* STEP 3.9's parameterList — its own condition verbatim, "if name is onerror and eventTarget is a
           Window object". BOTH CONJUNCTS, and the second is not decoration: `<img onerror>` is the commonest
           element handler on the web and takes ONE argument, while step 3.7's probe asked the parse question
           with this same pair, so a compile that dropped the Window conjunct would build a five-parameter
           function out of a body that was accepted as a one-parameter one. */
        five = (i >= 0 && strcmp(EH_NAME[i], "onerror") == 0 && is_win);
        if (!is_win) {
            JSValue doc, form;

            /* STEP 3.1 HAS TWO ARMS AND A TARGET THAT IS NEITHER IS NOT ONE OF THEM. `!is_win` does not imply
               element — a Document is neither — so the partition is asserted with the POSITIVE test rather
               than inferred from the absence of a Window. It cannot arise: an internal raw uncompiled handler
               is minted only by §8.1.8.1's attribute change steps, whose subject is an ELEMENT, and determine
               the target moves a body or frameset's WindowEventHandlers attributes onto the Window — so the
               only two objects that can hold one are the two arms this step names. */
            DCHECK(g_handler_terms != NULL,
                   "§8.1.8.1 step 3.1's partition was asked of a target on a host that registered none of "
                   "this section's defined terms — an element's inline handler cannot be compiled without "
                   "step 3.9's scope, and the scope is built out of terms only the HTML layer can answer");
            DCHECK(g_handler_terms->is_element(ctx, target),
                   "§8.1.8.1's get the current value of the event handler reached step 3 on a target that is "
                   "neither an element nor a Window — step 3.1 has no third arm, and an internal raw "
                   "uncompiled handler can only have been minted by the attribute change steps on an element "
                   "or moved to a Window by determine the target of an event handler");

            /* STEP 3.9's SCOPE, SUBSTEPS 3, 4 AND 5 — each ONE NewObjectEnvironment over the previous scope,
               so the carrier holds them OUTERMOST FIRST and handler_source emits one `with` head apiece. See
               that function's banner for why a `with` head IS the layer and why `this` is the only channel a
               layer cannot shadow. */
            carrier = JS_NewObjectProto(ctx, JS_NULL);
            CHECK(JS_IsObject(carrier),
                  "§8.1.8.1 step 3.9's scope carrier could not be allocated");

            /* SUBSTEP 3 — "If eventHandler is an element's event handler, then set scope to
               NewObjectEnvironment(document, true, scope)", where `document` is step 3.1's, the ELEMENT's node
               document. The condition is the step 3.1 arm this branch is: the substep's own note says
               "(Otherwise, eventHandler is a Window object's event handler.)", so there is nothing else it
               could be asking. */
            doc = g_handler_terms->node_document(ctx, target);
            DCHECK(JS_IsObject(doc),
                   "§8.1.8.1 step 3.1's `document` is not an object for a target its own is_element answered "
                   "yes for — every element has a node document, and a scope layer over a non-object is a "
                   "TypeError at the `with` head rather than the environment the step names");
            JS_SetPropertyUint32(ctx, carrier, 0, doc);
            layers |= EH_SCOPE_DOCUMENT;

            /* SUBSTEP 4 — "If form owner is not null, then set scope to NewObjectEnvironment(form owner, true,
               scope)", over step 3.5's form owner. NULL IS A POSITIVE ANSWER and not an absence to default
               past: "Otherwise, let form owner be null" is the standard's own arm for an element that has
               none, and it means this layer is NOT ADDED — which is why the head is dropped rather than
               emitted over undefined. */
            form = g_handler_terms->form_owner(ctx, target);
            if (JS_IsNull(form)) {
                JS_FreeValue(ctx, form);
            } else {
                DCHECK(JS_IsObject(form),
                       "§8.1.8.1 step 3.5's form owner is neither null nor an object — the term answers a form "
                       "element's wrapper or JS_NULL, and substep 4 layers whatever it answered");
                JS_SetPropertyUint32(ctx, carrier, 1, form);
                layers |= EH_SCOPE_FORM;
            }

            /* SUBSTEP 5 — "If element is not null, then set scope to NewObjectEnvironment(element, true,
               scope)". On this arm step 3.1 set element to eventTarget, so the condition holds and the layer
               is the target itself. INNERMOST, which is what makes `<button onclick="value = 'x'">` write the
               button's own IDL attribute instead of a global. */
            JS_SetPropertyUint32(ctx, carrier, 2, JS_DupValue(ctx, target));
            layers |= EH_SCOPE_ELEMENT;
        }

        /* EVERY BODY THAT REACHES THE COMPILE HAS PASSED STEP 3.7, AND THE HEADED SOURCE'S SOUNDNESS RESTS ON
           IT — see handler_source's banner. handler_current answers the RECORD only on its parsable arm, so
           this re-asks the same question of the same body with the same parameter list, and a disagreement is
           a caller that reached the compile some other way rather than a body that changed. It is dev-only and
           runs once per handler, because step 3.12's write is what makes the compile happen once. */
        DCHECK(handler_body_parsable(ctx, body, five),
               "§8.1.8.1 step 3.9 is compiling a body that is NOT parsable as a FunctionBody alone — step 3.7 "
               "is the whole of what stops `}); alert(1); ({` from closing the wrapper and running as program "
               "source, and the element arm's `with`-headed shape cannot carry js_parse_fn_ctor_source's "
               "end-of-input pin, so a body that never went through 3.7's probe is an escape");

        /* STEPS 3.8 AND 3.10 — the realm execution context pushed for the create and popped after — are the
           REALM this compile happens in, and the note beside 3.8 says so: "This is necessary so the subsequent
           invocation of OrdinaryFunctionCreate takes place in the correct realm." An instance of this engine
           is ONE origin-keyed agent cluster, so `ctx` IS settings object's realm for every target this walk
           can reach, and there is no second realm to push. That makes 3.8/3.10 a pair with nothing to perform
           rather than a pair that is skipped — the day a cross-realm target reaches here, the compile takes a
           realm argument and step_program_run's own `realm` parameter is the shape to copy. */
        src = handler_source(ctx, body, five, layers, &n);
        JS_FreeValue(ctx, body);
        /* STEP 3.9's OrdinaryFunctionCreate, PERFORMED AS THE EVALUATION OF A PROGRAM WHOSE VALUE IS THE
           FUNCTION — which is not a paraphrase of the step but the entry quickjs already has for it, and the
           one §20.2.1.1.1 CreateDynamicFunction reaches for the same reason. js_parse_fn_ctor_source emits
           `OP_put_loc eval_ret_idx; OP_get_loc eval_ret_idx; return`, so the program's completion value IS the
           closure the parenthesised FunctionExpression evaluates to.
           EACH FLAG DECIDES ONE THING AND NOTHING ELSE. JS_EVAL_TYPE_GLOBAL makes the program's own scope
           realm.[[GlobalEnv]], which is step 3.9's scope substep 2 and the whole of the scope where substeps
           3-5 add nothing, and it keeps the compile OFF the @S seam — §19.2.1.2 HostEnsureCanCompileStrings is
           performed by DIRECT and INDIRECT eval and by CreateDynamicFunction, never by a global program, and
           announcing this one would report every inline handler on every page as a code-execution sink.
           JS_EVAL_FLAG_TRAMP_CLOSURE hands the program CLOSURE back instead of running it here, which is what
           lets the call be a request. JS_EVAL_FLAG_FUNCTION_CTOR selects js_parse_fn_ctor_source's
           END-OF-INPUT pin and is CARRIED ONLY BY THE LAYERLESS SHAPE, because that entry's first act is
           js_parse_expect(s, '(') under a DCHECK that the next token is `function` — a source beginning with a
           `with` head is one it refuses by construction, and no flag makes it accept one.
           RETIRED TEXT, unquoted because it is this file's and not a standard's: this used to say the pin is
           what makes the wrapper unforgeable, and that was an OVER-CLAIM the headed shape refutes. What makes
           a wrapper unforgeable is that its body is a FunctionBody ALONE, and the algorithm establishes that
           at step 3.7, over the pinned probe, before either shape is built —
           `}); alert(1); ({` is rejected there and never reaches this line. The pin is still taken wherever it
           can be, because a second independent stop costs nothing; it is not what the argument rests on, and
           the DCHECK above is where the argument is checked.
           IT CANNOT FAIL, AND THAT IS ASSERTED RATHER THAN HANDLED: step 3.7 has already compiled this body
           with this parameter list under JS_EVAL_FLAG_COMPILE_ONLY and answered that it parses, and the heads
           this shape adds are this file's own fixed text over a carrier it built, so a parse error here is the
           two probes disagreeing about one text. */
        prog = JS_Eval(ctx, src, n, "<event handler>",
                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TRAMP_CLOSURE |
                       (layers == 0 ? JS_EVAL_FLAG_FUNCTION_CTOR : 0));
        free(src);
        JS_FreeValue(ctx, h);
        JS_FreeValue(ctx, map);
        if (JS_IsException(prog)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, carrier);
            DFAIL("§8.1.8.1 step 3.9's compile of an event handler body FAILED on a body step 3.7 had already "
                  "answered was parsable — the body and its parameter list are the text 3.7's pinned probe "
                  "accepted, and everything else in this source is fixed text this file wrote over a carrier "
                  "it built, so the two parses cannot disagree about one body");
            JS_FreeValue(ctx, in);   /* as above: no request is issued, so nothing else consumes it */
            JS_ThrowInternalError(ctx, "HTML §8.1.8.1 step 3.9's compile failed on a body step 3.7 accepted");
            return -1;
        }
        *stage = 1;
        /* THE RECEIVER IS THE GLOBAL OBJECT WHERE THE SCOPE HAS NO LAYERS, which is what a global program's
           evaluation is given — the same value step_program_run puts in its own request buffer's `this` slot
           — and THE CARRIER where it has them, because `this[0..2]` is how the heads name their objects.
           SWAPPING IT IS NOT OBSERVABLE TO THE HANDLER: the function expression is an ordinary function with
           thisMode non-lexical-this, so its own `this` is whatever the DISPATCH passes at invoke time and
           never the program's. What reads the program's `this` is the head text this file wrote. */
        recv = (layers == 0) ? JS_GetGlobalObject(ctx) : JS_DupValue(ctx, carrier);
        r = step_call_run(ctx, cphase, cb, cb_cap, prog, recv, 0, NULL, in, &fn, out_cb, out_argc);
        JS_FreeValue(ctx, recv);
        JS_FreeValue(ctx, carrier);   /* the request holds the layers through `recv`, which it dup'd into cb[] */
        JS_FreeValue(ctx, prog);   /* step_call_run dup'd both into cb[] */
    } else {
        DCHECK(*stage == 1, "§8.1.8.1 step 3's compile resumed in a stage it never parks in");
        r = step_call_run(ctx, cphase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 0, NULL, in, &fn,
                          out_cb, out_argc);
    }
    if (r > 0)
        return r;   /* parked evaluating the program; the resume re-enters at stage 1 */
    if (r < 0) {
        /* THE RECORD'S INVARIANT IS RESTORED HERE AND NOT ONLY AT THE CALLER: `stage` is zero exactly when no
           compile is in flight, and an abrupt one is not in flight. A caller that also clears it is agreeing
           with this, never the other way round. */
        *stage = 0;
        return -1;
    }
    *stage = 0;
    DCHECK(JS_IsFunction(ctx, fn),
           "§8.1.8.1 step 3.9's program evaluated to something that is not a function — the program is one "
           "parenthesised FunctionExpression, under any scope heads step 3.9 asked for, and a program's "
           "completion value is its last ExpressionStatement's whether or not that statement is inside a "
           "`with` body, so anything else means a second source element got in");
    /* STEP 3.12 — "Set eventHandler's value to the result of creating a Web IDL EventHandler callback function
       object whose object reference is function and whose callback context is settings object." The WRITE is
       what makes this compile happen ONCE: the next dispatch reads a function out of the map and step 3 does
       not apply at all. The CALLBACK CONTEXT is step 3.6's settings object, which this engine does not carry
       on a callback — one agent, one settings object, so nothing yet reads it back; it becomes a real field
       the day a callback can be invoked from another realm. */
    {
        JSValue map = handler_map(ctx, target, 0);

        DCHECK(JS_IsObject(map), "§8.1.8.1 step 3.12 has no handler map to write the compiled function to");
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, fn));
        JS_FreeValue(ctx, map);
    }
    *pout = fn;
    return 0;
}

/* §8.1.8.1's ACTIVATE AN EVENT HANDLER. Its steps 3-5 are "if eventHandler's listener is not null, then
   return", the one callback, and one add-an-event-listener — so the registration happens ONCE, the first time
   a handler for this type is set, and every later assignment changes what the marker RESOLVES TO rather than
   appending a second listener. That "once" is DOM §2.7's own dedup on (type, callback, capture), which is why
   this is one unconditional call and not a flag: the marker is a single runtime-wide object, so a second add
   for the same type finds the same triple already there.
   THE CALLBACK IS THE MARKER AND NOT THE HANDLER, which is §8.1.8.1's own note — "the callback is emphatically
   not the event handler itself. Every event handler ends up registering the same callback" — and is what keeps
   `el.onclick = a; el.addEventListener('click', b); el.onclick = c` running c before b. */
static void handler_activate(JSContext *ctx, JSValueConst target, const char *type)
{
    add_listener_with_type(ctx, target, g_handler_marker, type, /*capture*/ false, /*once*/ false,
                           /*passive*/ -1, /*signal*/ JS_UNDEFINED);
}

/* §8.1.8.1's DEACTIVATE AN EVENT HANDLER — "set eventHandler's value to null", then "if listener is not null,
   then remove an event listener". BOTH HALVES, which is the whole reason it is one algorithm: clearing the
   value alone leaves a marker in the list that resolves to null on every dispatch (a listener that costs a
   walk and does nothing), and removing the listener alone leaves a value the IDL getter would hand back for a
   handler that can no longer fire. `map` is the caller's handler map, BORROWED. */
static void handler_deactivate(JSContext *ctx, JSValueConst target, JSValueConst map, const char *type)
{
    JS_SetPropertyStr(ctx, (JSValue)map, type, JS_NULL);
    remove_listener_with_type(ctx, target, g_handler_marker, type, /*capture*/ false);
}

/* HTML §8.1.8.1's DETERMINE THE TARGET OF AN EVENT HANDLER, given `target` and the name at index `magic`.
 *
 * "Most of the time, the object that exposes an event handler is the same as the object on which the
 * corresponding event listener is added. However, the body and frameset elements expose several event handlers
 * that act upon the element's Window object, if one exists." That sentence is the whole of this function and it
 * is not a conformance detail: §13.2.7 "The end" step 9.5 fires `load` AT THE WINDOW, so a `load` handler left
 * on the body is a handler on an object that dispatch never visits. `document.body.onload = init` and
 * `<body onload="init()">` are two of the commonest ways a bundle starts, and both of them were dead.
 *
 * THE ANSWER IS OWNED and has three shapes, because the algorithm has three outcomes: the eventTarget itself
 * (steps 1 and 2), JS_NULL (step 3 — the caller must not fall back to the element, which is why this returns
 * null rather than the target), and the node document's relevant global object (step 4).
 *
 * A HOST THAT REGISTERED NO TERMS HAS NO BODY ELEMENTS, so step 1 answers "not a body element" and the whole
 * algorithm is the identity — the same answer event_target_is_window takes for a host with no tree, and exactly
 * right for a host that installs handlers on ports and signals with no document anywhere. */
static JSValue handler_determine_target(JSContext *ctx, JSValueConst target, int magic)
{
    JSValueConst global;

    DCHECK(magic >= 0 && magic < EH_COUNT, "an event handler was declared with a magic the list does not name");

    /* STEP 1: "If eventTarget is not a body element or a frameset element, then return eventTarget." */
    if (g_handler_terms == NULL || !g_handler_terms->is_body_or_frameset(ctx, target))
        return JS_DupValue(ctx, target);

    /* STEP 2: "If name is not the name of an attribute member of the WindowEventHandlers interface mixin AND
       the Window-reflecting body element event handler set does not contain name, then return eventTarget."
       Both sets, tested against the ONE X-list's mask — a body's `onclick` is neither, and stays on the body
       exactly as §4.3.1's own example ("an alert saying [object HTMLBodyElement] whenever the user clicks")
       requires. A prefix test over `on*` would have moved every one of the other seventy-odd. */
    if (!(EH_MASK[magic] & (EH_WINDOW | EH_WINDOW_REFLECTING)))
        return JS_DupValue(ctx, target);

    /* STEP 3: "If eventTarget's node document is not an active document, then return null." NULL is a real
       outcome and not an error: a body element in a `DOMParser` document, an XHR `responseXML` or a
       `createHTMLDocument` has no Window to act upon, so the getter answers null and the setter does nothing. */
    if (!g_handler_terms->node_document_is_active(ctx, target))
        return JS_NULL;

    /* STEP 4: "Return eventTarget's node document's relevant global object." */
    global = g_handler_terms->node_document_global(ctx, target);
    DCHECK(JS_IsObject(global),
           "§8.1.8.1's determine the target of an event handler reached step 4 with no relevant global object "
           "— step 3 has already established that the node document is an ACTIVE document, and an active "
           "document is the active document OF A NAVIGABLE, which has a Window");
    DCHECK(event_target_is_window(ctx, global),
           "§8.1.8.1's determine the target of an event handler answered with something that is not a Window "
           "— step 4's answer is a Document's relevant global object, and a handler registered anywhere else "
           "would be waiting on a dispatch that never visits it");
    return JS_DupValue(ctx, global);
}

/* §8.1.8.1's GETTER OF AN EVENT HANDLER IDL ATTRIBUTE — three steps, ONE `<ol>` with no nested list in it:
 * "Let eventTarget be the result of determining the target of an event handler given this object and name",
 * "If eventTarget is null, then return null", "Return the result of getting the current value of the event
 * handler given eventTarget and name". All of the depth is inside that third step, whose own step 3 is the
 * twelve-substep compile.
 *
 * IT IS A STEP MACHINE, AND THE REST POINT IS NOT THIS ALGORITHM'S — it is inherited from the one step it
 * ends in. Steps 3.8-3.12 produce their function by EVALUATING a program, which is a CALL, and quickjs.c's
 * JS_CallInternal DFAILs UNCONDITIONALLY on a bytecode body entered by C recursion below a live flow — it does
 * not ask what the body contains. So a plain C accessor cannot finish this getter, and the previous revision
 * crashed here saying so. What replaced the crash is not a way to make that call safe from C; it is this
 * function becoming the kind of thing that may make it, which is what §C-stack means by hooking a
 * continuation-holding builtin into the flow machinery rather than re-hosting it.
 *
 * THE DISPATCH WALK ALREADY DROVE THE SAME SUB-ALGORITHM, and this reaches it through the identical call.
 * handler_compile_run is a sub-algorithm of a step machine rather than a machine, so the record it needs — one
 * stage byte, a call phase and a request buffer — belongs to whichever machine drives it. The walk keeps those
 * in its own state as `ehc`/`cphase`/`cb`; this keeps them in its own, and the two never share one. That the
 * SAME function serves both is the point: a getter compiling a handler by a second route is two answers to
 * §8.1.8.1 step 3, and the day one of them gained step 3.11 the other would not have.
 *
 * THE MAGIC IS THE POOL ENTRY'S, WHICH IS WHY THERE ARE ~90 DECLARATIONS AND NOT ~90 CLOSURES. A machine's
 * magic slot is spent on its step id, so the ~90 handler attributes cannot be ~90 magics over one machine the
 * way they were over one C getter, and the index has to travel some other way.
 *   RETIRED ARGUMENT, rewritten rather than deleted because a reader who re-derives it will re-introduce it:
 * this said the index travels as CLOSURE DATA, minted by the named step-closure form so Web IDL §3.7.6's
 * "get onerror" survived. That was true while the family was defined at a raw JS_DefinePropertyGetSet, and it
 * is what the raw site cost — a closure mint carries data and the args pool's mint does not, so choosing the
 * closure was choosing to decide §3.7.6's descriptor and name at the call site and to state §3.5's security
 * kind nowhere at all.
 * The pool answers the same question one level up: `idl_getter_id_step` takes the magic AT THE DECLARATION and
 * `idl_step_magic` reads it back off the header, so each attribute is its own pool entry over ONE IdlStepDecl
 * and nothing has to ride on the function object at all. The name then comes from idl_mint_step, which
 * composes §3.7.6's prefix for every accessor in the engine through the one composer. */
#define EHG_STAGES(X)                                                                                          \
    X(EHG_GET, "HTML §8.1.8.1 the getter of an event handler IDL attribute steps 1-3 (determine the target, "   \
               "then get the current value, whose steps 3.8-3.12 evaluate the program that produces the "       \
               "handler and therefore park)")
enum { IDL_STEP_STAGE_BASE(EHG_STAGES) EHG_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EHG_STEPS[] = { EHG_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    /* NO `JSStepHdr` FIELD. A raw JSTrampStepDef's state begins with the header; a pool member's does not —
       the args machine owns the header and hands it to the body as its own argument, which is also how
       idl_step_magic reads this member's attribute index. */
    /* THE ONE-TIME PROLOGUE'S LATCH, A FIELD RATHER THAN THE STAGE, and it is not optional bookkeeping: a step
       state is js_mallocz'd, and a ZEROED JSValue IS THE INTEGER 0 rather than JS_UNDEFINED — the engine says
       so at tramp_step_state_new, where it declines to leave its own header slots to the same allocator. So
       every owned slot below is placed HERE, before the first thing that could leave through the teardown,
       which is §C-stack's rule that an init completes the state before anything that can throw. `ehc` cannot
       serve as this latch even though it is also zero on entry: handler_compile_run RESETS it to zero when the
       compile completes, so it answers "not started" again at exactly the moment the machine is finishing. */
    uint8_t   started;
    /* handler_compile_run's OWN stage byte, which it documents as zero exactly when no compile is in flight —
       so it is this machine's "have I parked inside step 3" test and there is no second flag to keep in step
       with it. */
    uint8_t   ehc;
    uint8_t   cphase;   /* the program evaluation's call phase, borrowed by handler_compile_run */
    /* STEP 1's eventTarget, OWNED ACROSS THE PARK. It is re-derivable from the receiver — determine the target
       runs no page code — but re-deriving it on the resume would ask a question the page's own handler may
       have changed the answer to between the park and the resume (a `<body>` removed from its document has no
       active document, so step 3 of determine the target answers null where it had answered the Window). The
       algorithm binds `eventTarget` once, at step 1, and every later step names THAT object. */
    JSValue   target;
    JSValue   result;   /* step 3's answer, OWNED until fini takes it */
    JSValue   cb[2];    /* [this, func] — step 3.9's program evaluation is called with no arguments */
} EhGetState;

static void ehg_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EhGetState *s = st;
    int i;

    v->val(ctx, &s->target);
    v->val(ctx, &s->result);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* THE ANSWER LEAVES THROUGH `presult`, WHICH IS WHY THERE IS NO `fini` HERE. A raw machine hands its result
   back from a teardown hook that is told whether to take it; a pool member writes it at the one exit that has
   one and the pool's own teardown discharges exactly what `ehg_visit` names. `s->result` is still a STATE
   slot rather than a local, because step 3's compile PARKS and the answer has to survive the park. */
static void ehg_done(EhGetState *s, JSValue *presult)
{
    *presult = s->result;
    s->result = JS_UNDEFINED;   /* HANDED OVER — the visit must not free what the caller now owns */
}

static int ehg_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EhGetState *s = st;
    int magic = idl_step_magic(hdr);
    JSValue h;
    int r;

    (void)argv;
    DCHECK(argc == 0, "§8.1.8.1's event handler getter was called with an argument — Web IDL §3.7.6 Attributes "
                      "mints an attribute getter with length 0 and declares no argument position for one");
    DCHECK(magic >= 0 && magic < EH_COUNT,
           "an event handler getter machine was declared over a magic the attribute list does not name");
    DCHECK(hdr->stage == EHG_GET, "§8.1.8.1's event handler getter resumed into a stage it does not have");

    if (!s->started) {
        int k;

        s->started = 1;
        s->target = s->result = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
    }

    /* THE RESUME COMES BACK INSIDE STEP 3, NOT AT STEP 1. `ehc` is non-zero exactly while the compile is in
       flight, so it is the whole of the routing: steps 1-2 are performed once, on the entry that finds it
       zero, and a park inside step 3.9 returns here to finish the same compile over the same target. */
    if (s->ehc != 0)
        goto compile;

    /* STEP 1 — "Let eventTarget be the result of determining the target of an event handler given this object
       and name." The receiver is the header's, which is where a machine's `this` lives.
       IT IS THE RAW RECEIVER, NOT A SUBSTITUTED ONE, and that is unchanged by this family becoming pool
       members: Web IDL §3.7.6 step 1.1.2.1's "the this value, if it is not null or undefined, or realm's
       global object otherwise" is a NAMED RESIDUAL of the args machine itself — idl_args.c states that this
       engine does not substitute — so what routing bought here is that the family is now COVERED BY that
       residual instead of sitting outside the machine with no statement about it at all. */
    s->target = handler_determine_target(ctx, hdr->this_val, magic);
    /* STEP 2 — "If eventTarget is null, then return null." */
    if (JS_IsNull(s->target)) {
        s->result = JS_NULL;
        ehg_done(s, presult);
        return JS_STEP_DONE;
    }
    /* STEP 3 — "Return the result of getting the current value of the event handler given eventTarget and
       name." handler_current performs that algorithm's code-free half and answers the INTERNAL RAW UNCOMPILED
       HANDLER when its steps 3.8-3.12 are owed; the predicate below is what tells that third answer from a
       value the page assigned, exactly as the dispatch walk's does. */
    h = handler_current(ctx, s->target, EH_TYPE[magic]);
    if (!handler_compile_owed(ctx, h)) {
        s->result = h;
        ehg_done(s, presult);
        return JS_STEP_DONE;
    }
    /* The RECORD is not the answer and must never leave this function — the compile below replaces it, and
       step 3.12 writes the function back to the map so a second read never reaches here at all. */
    JS_FreeValue(ctx, h);
compile:
    r = handler_compile_run(ctx, &s->ehc, &s->cphase, STEP_CB(s->cb), s->target, EH_TYPE[magic],
                            cb_result, &s->result, out_cb, out_argc);
    if (r > 0)
        return r;   /* parked evaluating the program; the resume re-enters above and jumps back to `compile` */
    if (r < 0) {
        /* THE ABRUPT ARM IS A THROW OUT OF THE GETTER AND NOT A NULL, and that is the one place this algorithm
           differs from the dispatch walk's use of the same sub-algorithm. §2.9 "inner invoke" step 2.11 gives
           the walk somewhere to put an exception — REPORT it and carry on down the listener list — because a
           dispatch has more listeners to run. A getter has no such step: Web IDL §3.7.6's attribute getter is
           `Return ? ...`, so an abrupt completion PROPAGATES to the page's own `el.onclick` expression, which
           is what a browser does and what answering null would hide. */
        return JS_STEP_ABRUPT;
    }
    DCHECK(JS_IsFunction(ctx, s->result),
           "§8.1.8.1 step 3's compile answered the getter with something that is not a function — step 3.12 "
           "writes a Web IDL EventHandler callback function object, and this getter hands its caller exactly "
           "what the next dispatch would invoke");
    ehg_done(s, presult);
    return JS_STEP_DONE;
}

static const IdlStepDecl EHG_DECL = {
    /* No release: `target`, `result` and the call pair are ehg_visit's, discharged with the rest of the state,
       and the compile's own stage byte and call phase are plain bytes that hold nothing. */
    ehg_step, sizeof(EhGetState), ehg_visit, NULL,
    "HTML §8.1.8.1 the getter of an event handler IDL attribute", EHG_STEPS
};

/* HTML §8.1.8.1's handler attributes are ordinarily pure state — assign a function, it is called when the event
   fires, and nothing else happens. The platform has ONE exception: §9.4.2 says setting `onmessage` on a
   MessagePort also STARTS the port, which is why a page that assigns onmessage never calls start() and a page
   that only uses addEventListener must. A general side-effect mechanism for a single member would be more
   machinery than the rule; instead the interested component registers here and decides for itself, by name and
   by its own brand test, which keeps this file from knowing what a MessagePort is. The slot itself is up with
   the other three another component claims, because event_target_init declares it as agent state. */
void event_target_set_handler_hook(void (*after_set)(JSContext *ctx, JSValueConst target, const char *name))
{
    /* ONE CLAIMANT. A second component setting this would replace the first with nothing anywhere recording
       that §9.4.2's port-start step had stopped running, which is a member that silently does half its job. */
    DCHECK(after_set == NULL || g_handler_set_hook == NULL,
           "a second component claimed HTML §8.1.8.1's handler-set hook — there is ONE, and the second claim "
           "silently replaces the first");
    g_handler_set_hook = after_set;
}

/* §8.1.8.1's SETTER OF AN EVENT HANDLER IDL ATTRIBUTE. Its step 1 is determine the target of an event handler
   and every step after it names THAT object — "Let handlerMap be eventTarget's event handler map", "activate an
   event handler given eventTarget and name" — so the map, the listener and the §9.4.2 hook all take it. Reading
   `this_val` for any one of them is what put a `load` handler on the body: the map would live on the element
   while the listener lived on the Window, or the reverse, and either half alone never fires.

   IT IS AN `IdlSetter` — a pool member's BODY, run once the assigned value has crossed the declaration — and
   not a JS_CFUNC_setter_magic function object. `magic` is therefore the POOL ENTRY'S, declared at
   handler_declare_members, rather than the function object's magic slot; `this_val` is the same raw receiver
   it always was, because §3.7.6's null-or-undefined substitution is a residual the args machine names and
   does not yet perform. What the pool adds ahead of this body is §3.5 Security's check, which
   idl_implementation_check runs for every member whose mint stated a kind.

   IT STAYS A PLAIN C BODY AND THAT IS THE SPEC'S ANSWER RATHER THAN A CONCESSION. A body is declared a MACHINE
   so it can suspend; none of these four steps can. The value is not coerced on the way in either — Web IDL
   §3.2.20 Nullable types returns null outright for a non-object under `[LegacyTreatNonObjectAsNull]` — so
   there is no page `toString` here to park on, which is the one thing a setter usually has and this one does
   not. See handler_declare_members for the declaration that states it. */
static JSValue js_handler_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *type;
    JSValue map, target;

    DCHECK(magic >= 0 && magic < EH_COUNT, "an event handler was declared with a magic the list does not name");
    type = EH_TYPE[magic];

    /* STEPS 1-2: "Let eventTarget be the result of determining the target … If eventTarget is null, return." */
    target = handler_determine_target(ctx, this_val, magic);
    if (JS_IsNull(target))
        return JS_UNDEFINED;

    map = handler_map(ctx, target, 1);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); JS_FreeValue(ctx, target); return JS_UNDEFINED; }
    /* THE TEST IS "IS IT AN OBJECT", AND WEB IDL IS WHY. The attribute's type is `EventHandler`, a NULLABLE
       callback function annotated `[LegacyTreatNonObjectAsNull]`, and §3.2.20 Nullable types converts a
       NON-OBJECT to null under exactly that annotation — which is what makes `el.onclick = "alert(1)"` and
       `el.onclick = 5` deactivate rather than throw, and is the whole of the legacy in the name. §3.2.19
       Callback function types then declines to throw for the non-callable OBJECT in the same case, so `{}` is
       stored and read back, and its TypeError comes at §3.12's invoke when the event fires.
       A page assigning a STRING is writing legacy markup-style code, which HTML compiles for a CONTENT
       attribute and never for this one: §3.2.20 has already made it null before this setter's steps begin. */
    if (JS_IsObject(val)) {
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, val));
        handler_activate(ctx, target, type);
    } else {
        handler_deactivate(ctx, target, map, type);
    }
    /* AFTER the handler is registered, for the reason event_target.h gives: §9.4.2's start() delivers what is
       already queued, and running it first would fire those events at a target with no listener yet.
       IT TAKES THE DETERMINED TARGET, because §9.4.2's requirement is stated over the object whose event
       handler this is. For a MessagePort the two are the same object — step 1 returns a port unchanged — but
       reading `this_val` here would be a second answer to a question step 1 has already settled. */
    if (g_handler_set_hook)
        g_handler_set_hook(ctx, target, EH_NAME[magic]);
    JS_FreeValue(ctx, map);
    JS_FreeValue(ctx, target);
    return JS_UNDEFINED;
}

/* HTML §8.1.8.1: an EVENT HANDLER CONTENT ATTRIBUTE is "a content attribute for a specific event handler", and
   which handlers have one is decided by the standard that EXPOSES them that way — never by this list's mere
   membership. That sentence used to read "that name set is exactly the list above", which is the claim the
   fourteen rows below refute. Trusted Types §3.8 Get Trusted Type data for attribute step 2 asks the question
   of every setAttribute: a handler content attribute demands a TrustedScript, so `el.setAttribute("onclick",
   s)` throws under `require-trusted-types-for 'script'` while `el.setAttribute("title", s)` does not. */
/* THE HANDLER LIST'S ROWS, ENUMERATED — every event handler IDL attribute, content attribute or not, off the
   one X-list rather than a second copy that would drift the first time a handler is added.
   IT IS AN ENUMERATION AND NOT A FILTER, which is the difference HTML §8.6.2's remove-unsafe needs: it appends
   "each attribute that is an event handler content attribute" to a configuration's removeAttributes list, and
   a caller that could only ask "is this one" can filter an allow-list it already has but can never build the
   deny-list the step describes.
   WHICH ROWS ARE CONTENT ATTRIBUTES IS STILL THIS FILE'S ANSWER — the caller walks these rows and asks
   event_target_handler_attribute_on_element per row. A pre-filtered enumeration would have been the shorter
   API and the worse one: it would have hidden the fact that the two sets DIFFER, which is the fact this
   component has to keep saying out loud, and it would have handed §8.6.2 an index space that means something
   different from every other index in this file. */
int event_target_handler_attribute_count(void) { return EH_COUNT; }

const char *event_target_handler_attribute_at(int i)
{
    DCHECK(i >= 0 && i < EH_COUNT, "an event handler content attribute was asked for by an out-of-range index");
    return EH_NAME[i];
}

int event_target_handler_attribute_index(const char *name, size_t name_len)
{
    int i;

    DCHECK(name != NULL || name_len == 0,
           "the event handler content attribute test was asked about a name that is a null pointer with a "
           "length — the two are one operand and a length over nothing is a read of whatever follows");
    /* ASCII case-insensitively: an attribute name reaching here has already been lowercased by DOM §4.9 step 2
       for an HTML element, but setAttributeNS performs no such lowercasing and `onClick` in an XML document is
       not an event handler content attribute — the compare is stated once here rather than at each caller. */
    for (i = 0; i < EH_COUNT; i++) {
        const char *n = EH_NAME[i];
        size_t k = 0;
        while (k < name_len && n[k]) {
            char a = name[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != n[k]) break;
            k++;
        }
        if (k == name_len && !n[k]) return i;
    }
    return -1;
}

/* THE PREDICATE IS THE LOOKUP AND THEN THE MEMBERSHIP, not a second walk of the same list. §8.1.8.1's
   attribute change steps need the ROW, because everything after step 1 is keyed on it; Trusted Types §3.8 has
   no element and needs the NAME-LEVEL question, which is the membership asked with `body_or_frameset` true —
   the union of §8.1.8.2's first and third tables is exactly the set of names that are content attributes on
   SOME element.
   IT RETURNED THE LOOKUP ALONE, which is the one-bit-two-questions defect and was a live wrong answer in both
   of this predicate's callers. Fourteen rows of the X-list carry neither EH_GLOBAL nor EH_WINDOW —
   `onreadystatechange`, `onvisibilitychange` (§8.1.8.2's FOURTH table: "must be supported on Document objects
   as event handler IDL attributes", IDL only), `onloadend`, `ontimeout`, `onsuccess`, `oncomplete`,
   `onblocked`, `onupgradeneeded`, `onversionchange`, `oncurrententrychange`, `onnavigate`, `onnavigateerror`,
   `onnavigatesuccess`, `ondispose` — and every one of them is an IDL attribute of an interface that is not an
   element. Answering yes for them made `el.setAttribute("onreadystatechange", s)` throw a TypeError under
   `require-trusted-types-for 'script'` where a browser sets an ordinary attribute, and made §8.6.2's
   remove-unsafe strip fourteen attributes a browser keeps — the second of which a page can SEE, since
   `sanitizer.get()` reports the configuration those names were appended to. */
bool event_target_is_handler_attribute(const char *name)
{
    int index;

    DCHECK(name != NULL, "the event handler content attribute test was asked about no name");
    index = event_target_handler_attribute_index(name, strlen(name));
    return index >= 0 && event_target_handler_attribute_on_element(index, true);
}

bool event_target_handler_attribute_on_element(int index, bool body_or_frameset)
{
    DCHECK(index >= 0 && index < EH_COUNT,
           "§8.1.8.2's content-attribute membership was asked of a row the handler list does not have");
    /* §8.1.8.2's FIRST table is the GlobalEventHandlers set — "must be supported by all HTML elements, as both
       event handler content attributes and event handler IDL attributes" — and its THIRD is the eighteen
       WindowEventHandlers names, whose content attributes are "exposed on all body and frameset elements".
       EH_WINDOW_REFLECTING adds nothing here on purpose: §8.1.8.2's SECOND table is a table of
       GlobalEventHandlers members, so EH_GLOBAL already carries all six and that bit decides only their
       TARGET. A row with neither bit — `onupgradeneeded` on an IDBOpenDBRequest, `onvisibilitychange` on a
       Document — is an IDL attribute of an interface that is not an element and is a content attribute
       nowhere. (`onmessage` stood here as the first example and is NOT one: it carries EH_WINDOW, so
       `<body onmessage="x">` is a handler exactly as §8.1.8.2's third table says.)
       CALLED WITH `body_or_frameset` TRUE THIS IS ALSO THE NAME-LEVEL SET, which is why
       event_target_is_handler_attribute is one line over this rather than a second reading of EH_MASK: the
       union of the two tables is the set of names that are content attributes on SOME element, and two
       readings of one mask are two things that can disagree. */
    return (EH_MASK[index] & EH_GLOBAL) != 0 || (body_or_frameset && (EH_MASK[index] & EH_WINDOW) != 0);
}

JSValue event_target_determine_handler_target(JSContext *ctx, JSValueConst exposed, int index)
{
    DCHECK(index >= 0 && index < EH_COUNT,
           "§8.1.8.1's determine the target of an event handler was asked for a row the handler list does not "
           "have");
    return handler_determine_target(ctx, exposed, index);
}

void event_target_deactivate_handler(JSContext *ctx, JSValueConst target, int index)
{
    JSValue map;

    DCHECK(index >= 0 && index < EH_COUNT,
           "§8.1.8.1's deactivate an event handler was asked for a row the handler list does not have");
    /* NO MAP MEANS NOTHING TO DEACTIVATE, and creating one here would be the opposite of the algorithm: an
       entry's initial value IS null, so a target that has never had a handler set is already in the state
       deactivate leaves it in, and minting a map would put a slot record on every element a `removeAttribute`
       ever touched. */
    map = handler_map(ctx, target, 0);
    if (JS_IsObject(map))
        handler_deactivate(ctx, target, map, EH_TYPE[index]);
    JS_FreeValue(ctx, map);
}

void event_target_set_uncompiled_handler(JSContext *ctx, JSValueConst target, int index,
                                         const char *body, size_t body_n)
{
    JSValue map, rec;

    DCHECK(index >= 0 && index < EH_COUNT,
           "an internal raw uncompiled handler was set for a row the handler list does not have");
    DCHECK(body != NULL,
           "§8.1.8.1's attribute change steps reached step 5 with a null value — step 4 is what a REMOVED "
           "attribute takes and it deactivates, so a null arriving here is a caller that ran step 5 for a "
           "removal and would store a handler whose body does not exist");
    map = handler_map(ctx, target, 1);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return; }
    rec = uncompiled_new(ctx, body, body_n);
    if (JS_IsException(rec)) { JS_FreeValue(ctx, map); return; }
    /* STEP 5.5 — "set eventHandler's value to the internal raw uncompiled handler value/location". It REPLACES
       whatever the entry held, callback object included: `div.onclick = f` followed by
       `div.setAttribute("onclick", "g()")` leaves ONE handler, which is g, exactly as the two spellings of one
       IDL attribute leave one. */
    JS_SetPropertyStr(ctx, map, EH_TYPE[index], rec);
    /* STEP 5.6 — "activate an event handler given eventTarget and localName". */
    handler_activate(ctx, target, EH_TYPE[index]);
    JS_FreeValue(ctx, map);
}

/* §8.1.8.1's ~90 ATTRIBUTES, DECLARED — both accessors of every row, into the one args pool, once per agent.
 *
 * THE GETTER IS A MACHINE AND THE SETTER IS A PLAIN C BODY, which is not an inconsistency but the two halves
 * answering different questions — and BOTH are pool members either way, so neither is a second mechanism.
 * §8.1.8.1's SETTER is four top-level steps (its step 4 is ONE step holding a nested list of four, which a
 * flat count reads as eight): determine the target, return if it is null, deactivate for a null value, and
 * otherwise write the handler map and activate a listener. Not one of them runs the page's code, and neither
 * does the CONVERSION above it: the type is `EventHandler`, a nullable callback annotated
 * `[LegacyTreatNonObjectAsNull]`, and Web IDL §3.2.20 Nullable types says of a value that is not an object
 * "then return the IDL nullable type T? value null" — an OBJECT TEST, never a coercion, so `el.onclick = {
 * toString(){ throw 1 } }` stores the object and throws nothing. So the setter has nothing to park on and
 * declares as a body. Its GETTER ends in get the current value, whose steps 3.8-3.12 EVALUATE a program, and a
 * call is the one thing a C accessor may not make below a live flow — so it declares as a machine.
 *
 * IDL_ANY IS THE DECLARED TYPE FOR THE SAME REASON. That arm is the pool's pass-through — unconverted, and
 * idl_args.h's own list of what takes it names a callback alongside `any` and an interface type. (Unquoted
 * because those are idl_args.h's words and not a standard's; the standard's sentence is §3.2.20's above.)
 * §3.2.20's arm is then performed where it always was, by js_handler_set's own `JS_IsObject` test, which is
 * one implementation of one rule rather than a declared type that would have to restate it. */
static void handler_declare_members(JSContext *ctx)
{
    int i;

    for (i = 0; i < EH_COUNT; i++) {
        g_handler_get_id[i] = idl_getter_id_step(ctx, &EHG_DECL, i);
        /* ONE ARGUMENT, WHICH IS WEB IDL §3.7.6 Attributes' OWN NUMBER for every attribute setter there is —
           idl_mint_accessor asserts the declaration derives it, so a setter declared with any other arity is
           an operation wearing an attribute's install. */
        g_handler_set_id[i] = idl_setter_id(ctx, IDL_ANY, /*null_to_empty*/ false, js_handler_set, i);
        /* DECLARED TO core/agent_state.h ROW BY ROW, because both things the registry does with a slot are per
           SLOT: it asserts each one is back at its pre-init value, and it is what PUTS it back — this release
           is derived from these ~180 declarations and event_target_free carries no line naming them. A single
           summary row would leave the other ~179 neither asserted nor undone. Nothing is printed for these on
           the clean path; the walk only speaks when a slot is still set after the release column ran. */
        agent_state_id("event_target", &g_handler_get_id[i],
                       "HTML §8.1.8.1's event handler IDL attribute getter machine, one per attribute");
        agent_state_id("event_target", &g_handler_set_id[i],
                       "HTML §8.1.8.1's event handler IDL attribute setter, one per attribute");
    }
}

/* WHICH MIXINS A MASK NAMES, ASKED OF THE X-LIST ITSELF — the one thing an install of the event handler IDL
 * attributes of HTML §8.1.8.1 Event handlers — the section that states them — cannot find out by succeeding.
 *
 * THE FAILURE IT ENDS IS SILENCE AND NOT A WRONG ANSWER. The entry below selects rows by ONE constant mask,
 * so a caller whose bit no row carries installs ZERO members and returns exactly as a caller that installed
 * eighty does: the prototype is built, the realm finishes, and the only reader downstream is
 * engine/idlgen.mjs going on reporting those members ABSENT — which is what it reports for an interface
 * nobody has started. An absent install and a completed one were different facts this call could not tell
 * apart, and the caller could not either, because there is nothing to test: the members it asked for are
 * missing in precisely the way a member nobody asked for is.
 *
 * IT IS THE HALF-WIRED STATE OF ADDING A BIT, WHICH IS WHY IT LANDS AHEAD OF THE NEXT BIT AND NOT WITH IT.
 * A bit is added in TWO places — the enum in event_target.h, and the rows of the handler list above — and the
 * install site is usually a third file. Land the enum and the install without the rows and the install is a
 * no-op; that is the intermediate revision a conversion split across two commits leaves behind, and it is the
 * revision a bisect stops on and charges to the commit under test. The set that needs a bit next is the one
 * declared by HTML §10.2.1.1 The WorkerGlobalScope common interface — six event handler IDL attributes that
 * WorkerGlobalScope writes in its own interface, so no existing bit can carry them: every bit that holds any
 * of the six holds dozens of names beside them.
 *
 * BOTH OPERANDS ARE THIS FILE'S AND NEITHER IS A LITERAL COPIED FROM THE OTHER: the mask the caller passed,
 * and the union DERIVED from EH_MASK at the moment of the check. A restated constant would be a second copy
 * of the list's own arithmetic, and a mask compared against the constant it was spelled from is a comparison
 * of a thing with itself — an assert whose two sides cannot disagree.
 *
 * THE STRAY BITS ARE PRINTED IN DECIMAL BECAUSE THAT IS THE ADDRESS. This is a shared entry with twenty-odd
 * callers, so its abort stamps THIS file's line for every one of them and the message can name no site. A
 * mask bit is per-interface by construction, so the VALUE identifies the caller better than a line would,
 * and event_target.h's bit enum spells every bit as a decimal literal — so the number in the message greps
 * straight to the bit that names nothing. */
#if APICLIENT_DEV
static void eh_assert_mask_named(int mask)
{
    int carried = 0, i;

    for (i = 0; i < EH_COUNT; i++)
        carried |= EH_MASK[i];
    DCHECK(mask != 0,
           "event handlers were installed with a mask naming no mixin at all — a mask IS the set an "
           "interface's IDL declares, and an interface that declares no event handler does not reach this "
           "entry, so a zero is a caller that computed a set and got nothing rather than one that wanted "
           "nothing");
    DCHECKF((mask & ~carried) == 0,
            "event handlers were installed with mask %d, whose bits %d are carried by no row of this file's "
            "handler list — every member those bits name is silently NOT installed, and this call returns "
            "exactly as a successful one does. Grep event_target.h's bit enum for that decimal literal: "
            "either the bit was added to the enum and the rows it is meant to select were never given it, or "
            "the caller names a bit whose rows have gone",
            mask, mask & ~carried);
}
#define EH_ASSERT_MASK_NAMED(m) eh_assert_mask_named(m)
#else
#define EH_ASSERT_MASK_NAMED(m) ((void)0)
#endif

void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask)
{
    int i;

    DCHECK(JS_IsObject(target), "event handlers were installed on something that is not an object");
    EH_ASSERT_MASK_NAMED(mask);
    for (i = 0; i < EH_COUNT; i++) {
        if (!(EH_MASK[i] & mask))
            continue;
        DCHECK(g_handler_get_id[i] >= 0 && g_handler_set_id[i] >= 0,
               "an event handler attribute was installed before event_target_init declared its accessors");
        /* AN ORDINARY INSTALLED ATTRIBUTE, through the installer every other accessor in the engine uses.
           WHAT THE RAW JS_DefinePropertyGetSet DECIDED FOR ITSELF AND THIS DOES NOT: §3.7.6's descriptor
           ([[Enumerable]] true, [[Configurable]] true) is now the installer's one answer rather than this
           line's; §3.7.6's accessor NAME is composed at idl_mint_step for every accessor in the engine, so
           "get onclick" and "set onclick" cannot drift from the rest; and the mint states §3.5 Security's
           kind, which is what HTML §7.2.1.1 Integration with IDL matches against CrossOriginProperties's
           [[NeedsGetter]]/[[NeedsSetter]] — these ARE Window attributes, and until this they were installed on
           the global with no such statement, so a cross-origin read of one answered out of the reading realm
           where a browser throws SecurityError. The consumer is idl_implementation_check, which calls
           window_proxy_security_check for every member whose mint stated a kind and for no other. */
        idl_install_accessor_step(ctx, target, EH_NAME[i], g_handler_get_id[i], g_handler_set_id[i]);
    }
}

/* §2.9 "invoke" step 8: the walk runs over a CLONE of the listener list. That matters more here than in a browser — the
   walk suspends across every listener, so one that adds or removes a listener has arbitrarily long to do it.
   ONE list walk, so the engine's own firing and the page's dispatchEvent can never disagree about which
   listeners a target has; what differs between them is only how each listener is DELIVERED. */
static JSValue listener_snapshot(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue arr = listener_list(ctx, target, type, 0), copy = JS_NewArray(ctx);
    uint32_t len, i, k = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return copy; }
    len = arr_len(ctx, arr);
    /* The RECORDS, copied as they stand. Resolving the handler marker and filtering by the capture flag both
       belong to the walk: §2.9's "inner invoke" is where the spec does them, and the walk visits one list
       TWICE (once per direction) with a different answer each time. */
    for (i = 0; i < len; i++)
        JS_SetPropertyUint32(ctx, copy, k++, JS_GetPropertyUint32(ctx, arr, i));
    JS_FreeValue(ctx, arr);
    return copy;
}

/* §2.9 "inner invoke" step 2: `once` REMOVES the listener from the live list before it is called, so a
   listener that re-enters the same dispatch does not see itself. It is removed from the LIVE list, never from
   the snapshot the walk is iterating — the snapshot is what makes a removal during dispatch not skip a
   sibling, which is the whole reason §2.9 takes one. */
static void listener_remove_record(JSContext *ctx, JSValueConst target, const char *type, JSValueConst rec)
{
    JSValue map = listener_map(ctx, target, 0), arr, kept;
    uint32_t len, i, k = 0;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return; }
    len = arr_len(ctx, arr);
    kept = JS_NewArray(ctx);
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(rec)) { listener_mark_removed(ctx, e); JS_FreeValue(ctx, e); }
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
}


/* HTML §6.5 Activation behavior of elements' `click()` STEP 2, 3 AND 5's flag: "Each element has an associated
   click in progress flag, which is initially unset."
   ABSENT IS UNSET, and that is the standard's own sentence rather than a default filling a hole — an element
   nobody has clicked has no slot, which is exactly the state step 2 tests for and step 5 restores.
   IT IS READ AS AN OWN SLOT, never as a property LOOKUP, for the reason listener_map states: a miss on a page
   object walks the prototype chain into the solver's absent-state seam, which MINTS a concolic for the name.
   An unknown here would fork the guard — one world where a click is already in progress and one where it is
   not — off a fact this engine wrote itself and therefore knows. An internal slot is by definition an own
   slot. */
static bool click_in_progress(JSContext *ctx, JSValueConst el)
{
    JSAtom k;
    JSValue v;
    bool set;

    DCHECK(g_ready, "HTML §6.5's click in progress flag was read before event_target_init minted its key");
    k = JS_ValueToAtom(ctx, g_click_flag_key);
    CHECK(k != JS_ATOM_NULL, "HTML §6.5's click in progress flag key could not be interned");
    if (JS_GetOwnSlot(ctx, &v, el, k) <= 0)
        v = JS_FALSE;
    JS_FreeAtom(ctx, k);
    set = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return set;
}

/* Steps 3 and 5's WRITE. A [[DefineOwnProperty]] rather than a storage poke, because the whole point of putting
   the flag on the wrapper is that the COW delta captures it: the define records the slot's pre-write state
   (absent, for the first click) in the running flow's delta, so an arm forked between step 3 and step 5 unsets
   its own copy and a sibling that never entered click() still reads unset.
   CONFIGURABLE AND WRITABLE, for the reason constraint_validation.c gives for the same shape: the slot is
   written again at every click, and one defined with no flags makes the second write a silent no-op — which
   here would be step 5 failing to unset and the element never clickable again. */
static void click_in_progress_set(JSContext *ctx, JSValueConst el, bool on)
{
    JSAtom k;
    int ok;

    DCHECK(g_ready, "HTML §6.5's click in progress flag was written before event_target_init minted its key");
    k = JS_ValueToAtom(ctx, g_click_flag_key);
    CHECK(k != JS_ATOM_NULL, "HTML §6.5's click in progress flag key could not be interned");
    ok = JS_DefinePropertyValue(ctx, (JSValue)el, k, JS_NewBool(ctx, on),
                                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_FreeAtom(ctx, k);
    DCHECK(ok > 0,
           "HTML §6.5's click in progress flag would not stick to the element. The only way a define of a "
           "configurable own slot is refused is a target that is NOT EXTENSIBLE, which a page reaches with "
           "Object.preventExtensions/seal/freeze on an element — so what is missing is internal slots that "
           "survive it, which this engine models as own properties throughout (the listener map and "
           "§4.10.21.1's custom validity message have the same shape). Build that, or a sealed element's "
           "step 3 never sets the guard step 2 reads, and a click handler calling click() recurses without "
           "end");
}

/* §2.9 DISPATCH, as a machine — and the reason dispatchEvent could not exist before.
 *
 * The spec makes dispatch SYNCHRONOUS and makes its return value depend on what the listeners did: it answers
 * `!canceled`, so `if (!el.dispatchEvent(ev)) { … }` is how a page asks whether anything called preventDefault.
 * Neither of the two obvious implementations can answer that. Calling the listeners from C is the
 * drive-to-completion this engine aborts on — a listener body holds loops, awaits and concolic branches.
 * Enqueueing them as jobs answers before any of them has run, so the answer would always be "not cancelled".
 *
 * So each listener is a CALL REQUEST: the machine parks on it, the listener runs as ordinary preemptible page
 * code at whatever depth it likes, and the machine resumes at the listener it was on with `i` as its cursor.
 * That is the same shape every continuation-holding builtin in the engine has, which is why this needs no
 * machinery of its own beyond the state.
 *
 * THE LIST IS SNAPSHOT FIRST, which the spec requires and which matters here more than in a browser: a listener
 * that runs mid-walk can add or remove listeners, and this walk is suspended across every one of them. */
enum { DISPATCH_ARG = 0, CLICK_SYNTH = 1, DISPATCH_PAIR = 2 };

/* WHERE THIS MACHINE RESTS, AS §2.9 NUMBERS IT — read off the live standard rather than remembered, because the
   labels it carried before named steps that do not exist (there is no step 11.1; the activation behaviour is
   step 12.1, and the two listener loops are 6.13 and 6.14 inside the ONE `if` that step 6 is).
   It can suspend at three points: while WALKING the tree that makes the path (a page-sized walk, so it yields
   between parents), inside a listener, and inside the activation behaviour. */
#define DISPATCH_STAGES(X) \
    X(DISPATCH_INIT, "HTML §6.5 Activation behavior of elements' click() steps 1-3 for a synthetic click (the " \
                     "disabled-form-control return, the click in progress flag's test and set), Web IDL " \
                     "§3.2.15 Interface types' conversion of the `Event event` argument, DOM §2.7 " \
                     "Interface EventTarget's dispatchEvent(event) method steps 1-2 (its two-condition " \
                     "InvalidStateError, and isTrusted), then DOM §2.9 dispatch steps 1-6.8 (the dispatch " \
                     "flag, the target override, the relatedTarget retargeted against the target and the " \
                     "step 6 condition it decides, the target's own path item, whether the target is the " \
                     "activation target, whether it is an assigned slottable, and the first get the parent)") \
    X(DISPATCH_PATH, "DOM §2.9 dispatch steps 6.9-6.11 (walk get the parent, appending an event path ITEM per " \
                     "ancestor — retargeting the target, the relatedTarget and the touch targets at each " \
                     "shadow boundary and recording the closed-tree flags — one parent per yield, because the " \
                     "tree is the page's size)") \
    X(DISPATCH_CAPTURE, "DOM §2.9 dispatch step 6.13 (invoke each path item in REVERSE order with phase " \
                        "\"capturing\" — parked on the listener being called)") \
    X(DISPATCH_BUBBLE, "DOM §2.9 dispatch step 6.14 (invoke each path item in order with phase \"bubbling\" — " \
                       "parked on the listener being called)") \
    X(DISPATCH_ACTIVATION, "DOM §2.9 dispatch step 12.1 (the activation target's activation behaviour, run " \
                           "after the whole walk and only when nothing cancelled)")
enum { DISPATCH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DISPATCH_STEPS[] = { DISPATCH_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSDispatchState {
    JSStepHdr hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   cphase;    /* the call request's own phase, so a stage can hold a call across a suspension */
    uint32_t  i, n;      /* THE RESUME POINT: the listener being called, and how many there are */
    uint32_t  ti, tn;    /* THE OTHER: how far into the current PASS, and how long the whole path is */
    /* §2.9 step 6.4's isActivationEvent, decided once at step 6.4 and read again at 6.9.6.1 for every ancestor. */
    uint8_t   is_activation;
    /* HTML §6.5's click() STEP 5 DEBT, and it is a POSITIVE STATEMENT rather than a copy of the target: this
       holds an element EXACTLY WHEN step 3 set that element's click in progress flag and step 5 has not yet
       unset it. So the CLICK_SYNTH entries that returned at step 1 or step 2 leave it undefined and owe
       nothing, and no other entry into this machine can ever owe anything — which is what lets the unset be
       one unconditional line in the teardown instead of a condition restating the three arms. */
    JSValue   click_el;
    /* THE ACTIVATION BEHAVIOUR'S OWN SUSPENSION. §4.6.3's is a navigation and a navigation fetches, so the
       behaviour is a step like everything else that can wait on the host: `aphase` is its resume point and
       `areq` the host request it is waiting on. They live here because the machine that can park is this one. */
    uint8_t   aphase;
    uint32_t  areq;
    /* THE LISTENER'S OWN `passive`, held across the call: §2.9 "inner invoke" RAISES the event's in-passive
       listener flag before the listener runs and lowers it after, so the machine has to remember which way to
       put it back when it resumes — the record it read it from is gone by then. */
    uint8_t   in_passive;
    /* THE CALLBACK'S OPERATION LOOKUP. A callback INTERFACE that is not callable has `handleEvent` read off it
       per invocation, and that read is the page's code — so the object survives the suspension on the state and
       `lphase` says a read is outstanding. Two markers rather than one because a listener can suspend TWICE: on
       its operation lookup and then on the call. */
    uint8_t   lphase;
    /* §2.9 "inner invoke" step 2.11's "IF THIS THROWS AN EXCEPTION, REPORT IT" — the walk does not unwind, it
       REPORTS and continues, and reporting fires an `error` event at the global, which runs the page's code.
       So the exception survives on the state and the report is a sub-request with its own work record.
       `reporting` is its resume marker, the third this walk needs: a listener can suspend on its operation
       lookup, on its own body, and on the report of what its body threw. */
    uint8_t   reporting;
    /* HTML §8.1.8.1's EVENT HANDLER PROCESSING ALGORITHM, for the one listener kind that is not a listener.
       `eh_index` names which event handler IDL attribute the marker just resolved to (-1 for an ordinary
       listener), because §8.1.8.1 takes the attribute's NAME and the walk resolved the slot by TYPE; `eh` is
       that algorithm's own record, and its non-zero stage is this walk's FOURTH resume marker — a handler can
       suspend on its body and again on the Web IDL coercion of what it returned, and neither of those is a
       call the `cphase` marker alone can distinguish from "between listeners". */
    int16_t   eh_index;
    EventHandlerWork eh;
    /* HTML §8.1.8.1's GET THE CURRENT VALUE steps 3.8-3.12, which is this walk's FIFTH resume marker and the
       only one that fires BEFORE a handler has been invoked at all. A handler written in markup is an
       uncompiled record until the first dispatch reads it, and turning it into a function EVALUATES a program
       — a call, so a park — so the walk can suspend between resolving the marker and having anything to
       invoke. `ehc` is non-zero exactly across that, and it is a marker of its own rather than a value of
       `eh.stage` because the two are different algorithms: `eh` is the event handler PROCESSING algorithm's
       invocation, and this is the COMPILE that produces what that algorithm invokes. */
    uint8_t   ehc;
    /* §2.9 step 6.7's SLOT-IN-CLOSED-TREE, and step 6.10's CLEARTARGETS. Both are one-bit walk state that has
       to survive a park between two ancestors, which is the whole reason they are on the state and not on the
       C stack of a loop that does not exist. */
    uint8_t   slot_in_closed_tree;
    uint8_t   clear_targets;
    ReportExceptionWork rep;
    JSValue   exc;       /* what the listener threw, held across the report (owned) */
    JSValue   lcb;       /* the listener's callback object, held across its operation lookup (owned) */
    JSValue   type;      /* the event's type, resolved once per target and needed by `once` (owned) */
    JSValue   path;      /* §2.9's propagation path — a list of event path ITEMS (owned) */
    JSValue   cur;       /* the target whose listeners are running, and the WALK's frontier while step 6.9
                            builds the path (owned) */
    /* §2.9's `target` LOCAL, which the walk MOVES: step 6.9.8.1 sets it to the parent every time the walk
       crosses out of a shadow tree, and every later item is appended with the shadow-adjusted target that
       follows. It is not `cur` — `cur` is where the walk IS, this is what the walk currently calls the target —
       and conflating them is how every item ends up with path[0] as its target. (owned) */
    JSValue   tgt;
    /* §2.9 step 6.6's SLOTTABLE: the node whose assigned slot the next get the parent will answer with, so that
       step 6.9.1 can recognise that the parent it was handed is that slot and raise slot-in-closed-tree when
       the slot's tree is closed. Null except across exactly that one hop. (owned) */
    JSValue   slottable;
    /* §2.9's ACTIVATION TARGET: the nearest entry of the path, TARGET FIRST, that has an activation behaviour.
       Picked while the path is built and run after the walk — see event_target.h. (owned) */
    JSValue   act;
    JSValue   arr;       /* that target's listener list SNAPSHOT (owned) */
    JSValue   ev;        /* the event (owned) */
    JSValue   result;    /* !canceled (owned) */
    /* THE CALL REQUEST BUFFER: [this, callee, arguments…]. SEVEN slots, because HTML §8.1.8.1 step 5 invokes
       an `OnErrorEventHandler` with FIVE arguments — `window.onerror` gets (message, filename, lineno, colno,
       error) where an `addEventListener("error", …)` listener gets the one ErrorEvent — and the buffer a
       request parks in has to hold the widest invocation this walk can make, not the commonest. */
    JSValue   cb[7];
} JSDispatchState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here because a fork mid-listener must not hand two arms one
   invocation — that is exactly what the visit contract is for. */
static void js_dispatch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDispatchState *s = st;
    int k;
    v->val(ctx, &s->path);
    v->val(ctx, &s->type);
    v->val(ctx, &s->lcb);
    v->val(ctx, &s->exc);
    report_exception_work_visit(ctx, &s->rep, v);
    event_handler_work_visit(ctx, &s->eh, v);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->tgt);
    v->val(ctx, &s->slottable);
    v->val(ctx, &s->act);
    v->val(ctx, &s->arr);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->result);
    v->val(ctx, &s->click_el);
    /* DERIVED FROM THE ARRAY, never a literal beside it: quickjs-step.h's paragraph on this is about exactly
       the buffer below, which has now grown once — a visit one slot short leaves a live value the fork never
       dups, and it fails nowhere near here. */
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_dispatch_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDispatchState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;

    if (take_result) s->result = JS_UNDEFINED;
    /* HTML §6.5 Activation behavior of elements' click() STEP 5: "Unset this element's click in progress flag."
       HERE BECAUSE THIS IS THE ALGORITHM'S OWN LAST STEP, not because the teardown is a convenient place —
       quickjs-step.h's contract for `fini` names "a re-entrancy guard lowered" as one of the things a machine
       "owes on the way out", and the click in progress flag is exactly that. Step 4 is the whole dispatch, so
       step 5 is whatever runs after it, and this is the one point every exit from step 4 passes through: the
       ordinary completion, and the abrupt one (`catches_abrupt`) that the activation behaviour at step 12.1 can
       still raise. Written at the normal terminal alone it would leave the flag SET for ever on the abrupt
       path, and the element would never accept another click() — a page-visible dead element from a failure
       that had nothing to do with it.
       IT READS AN OWNED VALUE AND FREES NOTHING: the declaration names click_el, so tramp_step_state_free_1
       releases it after this returns, and a free here would be the second list this engine forbids. */
    if (JS_IsObject(s->click_el))
        click_in_progress_set(ctx, s->click_el, false);
    /* §8.1.4.6 step 5's FLAG, if the dispatch was abandoned inside a report. It is not a reference, so no
       declaration names it; the report record's references ARE named by js_dispatch_visit, which is why this
       is the unlock and not the whole release. */
    report_exception_work_unlock(ctx, &s->rep);
    return r;
}

/* §2.9's GET THE PARENT, asked of whoever owns the tree. It answers null for an EventTarget with no tree at all
   — §2.7's own get the parent, which `new EventTarget()` and an AbortSignal both have — so this file needs no
   "is it in the tree" question and no window special case: a DOCUMENT's get the parent IS the window (unless the
   event is `load`), which is a fact about documents and belongs where documents are known.
   That special case used to live here as "append the realm's global whenever the ancestor hook answered an
   array", which put the window above a DETACHED node as well, and above a document dispatching `load`. */
static JSValue dispatch_get_parent(JSContext *ctx, JSValueConst target, JSValueConst ev)
{
    JSValue parent;

    if (!g_tree)
        return JS_NULL;   /* a host with no tree: §2.7's default, a path of one */
    parent = g_tree->get_parent(ctx, target, ev);
    DCHECK(JS_IsNull(parent) || JS_IsObject(parent),
           "§2.9's get the parent answered with something that is not an EventTarget and is not null — the "
           "walk appends whatever it is handed to the event path and then invokes listeners on it");
    return parent;
}

/* THE SHADOW TERMS, EACH WITH THE ANSWER A HOST WITH NO TREE HAS. A host that registered no tree has no
   document, so nothing in it is a node, a slot, a shadow root or an ancestor of anything — which is not a
   fallback but the same §2.7 default that gives such a host a path of one. Written once here so no step of the
   walk below carries a `g_tree &&` of its own. */
static bool dispatch_is_window(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->is_window(ctx, t);
}

static bool dispatch_is_slot(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->is_slot(ctx, t);
}

static bool dispatch_is_assigned_slottable(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->is_assigned_slottable(ctx, t);
}

/* §4.8's mode of `t` ITSELF — "invocationTarget is a shadow root whose mode is closed", which is "append to an
   event path" step 4's root-of-closed-tree. */
static bool dispatch_is_closed_shadow_root(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->shadow_root_mode(ctx, t) == EVENT_TREE_SHADOW_CLOSED;
}

/* The mode of `t`'s ROOT, which is a different question and is asked at two different steps: step 6.9.1.3's
   "parent's root is a shadow root whose mode is closed", and step 6.11's "is a node whose root is a shadow
   root" (`want_closed` false, which every mode but NOT_SHADOW_ROOT satisfies). */
static bool dispatch_root_is_shadow_root(JSContext *ctx, JSValueConst t, bool want_closed)
{
    JSValue root;
    int mode;

    if (!g_tree)
        return false;
    root = g_tree->root(ctx, t);
    mode = g_tree->shadow_root_mode(ctx, root);
    JS_FreeValue(ctx, root);
    return want_closed ? mode == EVENT_TREE_SHADOW_CLOSED : mode != EVENT_TREE_NOT_SHADOW_ROOT;
}

/* IS THIS EVENTTARGET THAT ONE. Platform objects have identity — a node's wrapper is the same object for as
   long as the node lives — so "target is relatedTarget" and "parent is relatedTarget" are pointer questions.
   Written once because BOTH of §2.9's uses compare against a POTENTIAL event target, and `null is null` is not
   the answer either of them wants: step 6's condition is about an event with a relatedTarget, and step 6.9.7
   ends the walk at the ancestor that IS one. */
static bool same_target(JSValueConst a, JSValueConst b)
{
    return JS_IsObject(a) && JS_IsObject(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/* DOM §4.8's RETARGETING ALGORITHM. See event_target.h for why it is a component function and not four inline
   copies. It is a LOOP because one climb is not enough: a node inside a shadow tree inside a shadow tree is
   retargeted to the outer host, and each iteration re-asks all three of step 1's disjuncts about the host it
   just climbed to. */
JSValue event_target_retarget(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue cur, broot;
    bool b_is_node;

    DCHECK(JS_IsObject(a) || JS_IsNull(a),
           "§4.8's retargeting was given an A that is not a potential event target — the answer is handed to a "
           "listener as `event.relatedTarget`, and undefined is neither an EventTarget nor null");
    DCHECK(JS_IsObject(b) || JS_IsNull(b),
           "§4.8's retargeting was given a B that is not a potential event target — B is who A is about to be "
           "made visible to, and step 1's third disjunct asks whether B is a node");
    cur = JS_DupValue(ctx, a);
    if (!g_tree)
        return cur;   /* a host with no tree: nothing is a node, so step 1's first disjunct returns A at once */
    /* "B is a node", asked ONCE: B does not move, and the tree answers a null root for everything that is not
       a node — the same question the walk uses to tell a Window from a node without a second hook. */
    broot = g_tree->root(ctx, b);
    b_is_node = !JS_IsNull(broot);
    JS_FreeValue(ctx, broot);
    for (;;) {
        JSValue root, host;

        root = g_tree->root(ctx, cur);
        /* step 1, first disjunct: "A is not a node" — a Window, an AbortSignal, a plain `new EventTarget()`. */
        if (JS_IsNull(root)) {
            JS_FreeValue(ctx, root);
            return cur;
        }
        /* second: "A's root is not a shadow root" — A is in the document tree, so it is visible to everything
           in it and is reported as itself. */
        if (g_tree->shadow_root_mode(ctx, root) == EVENT_TREE_NOT_SHADOW_ROOT) {
            JS_FreeValue(ctx, root);
            return cur;
        }
        /* third: "B is a node and A's root is a shadow-including inclusive ancestor of B" — B is INSIDE the
           same shadow tree, so it may already see A and nothing is hidden from it. */
        if (b_is_node && g_tree->is_shadow_including_inclusive_ancestor(ctx, root, b)) {
            JS_FreeValue(ctx, root);
            return cur;
        }
        /* step 2: "set A to A's root's host", and go round again. */
        DCHECK(g_tree->shadow_host != NULL,
               "§4.8's retargeting has to climb from a shadow root to its HOST and the DOM registered no "
               "`shadow_host` in its EventTargetTree — BUILD IT: dom/node.c's NODE_EVENT_TREE must answer §4.8's "
               "host the way it already answers `root` and `shadow_root_mode`, by wrapping "
               "shadow_root_host(node_of(target)) and answering JS_NULL for anything that is not a shadow root. "
               "Until it does, an event whose relatedTarget or touch target is inside a shadow tree has no "
               "object to be retargeted TO, and this is the step that would have to invent one");
        host = g_tree->shadow_host(ctx, root);
        JS_FreeValue(ctx, root);
        DCHECK(JS_IsObject(host),
               "§4.8's retargeting climbed to a shadow root's host and the tree answered with none — §4.8's "
               "attach a shadow root gives every shadow root a host, so a null here is a shadow root whose host "
               "the tree lost, and the object it was hiding would be reported as null");
        JS_FreeValue(ctx, cur);
        cur = host;
    }
}

/* WEB IDL §3.2.15's "If V implements I" OVER THIS INTERFACE — see event_target.h. It is the whole of the type
   test and it is SEPARATE from the conversion below because a declared argument position asks only this half:
   §3.2.20's null rule is resolved by the argument machine before any brand is read, so a position declared
   `EventTarget?` needs the predicate and nothing else. Both halves are then one statement of the walk. */
bool event_target_is_value(JSContext *ctx, JSValueConst v)
{
    JSValue p, target;
    bool ok = false;

    /* THE WALK NEVER TOUCHES A PROXY. JS_GetPrototype on one runs its getPrototypeOf trap — the page's code,
       from inside a C activation — and a Proxy is not a platform object implementing the interface anyway, so
       a link that is one ends the walk instead of being asked. */
    if (!JS_IsObject(v) || JS_IsProxy(v))
        return false;
    target = event_target_proto(ctx);
    p = JS_GetPrototype(ctx, v);
    while (JS_IsObject(p) && !JS_IsProxy(p)) {
        JSValue next;
        if (JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(target)) { ok = true; break; }
        next = JS_GetPrototype(ctx, p);
        JS_FreeValue(ctx, p);
        p = next;
    }
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, target);
    return ok;
}

/* WEB IDL §3.2.15 Interface types' `EventTarget?` MEMBER, READ OFF THE CONVERTED DICTIONARY — see
   event_target.h. The BRAND is the declaration's at both of this type's dictionary members now: FocusEventInit's
   and MouseEventInit's `relatedTarget` are IDL_INTERFACE_NULLABLE carrying `.iface_is = event_target_is_value`
   and `.iface_name = "EventTarget"`, which is §3.2.15's `I` stated as the realm-taking predicate the walk
   resolves through idl_member_implements. So §3.2.17 Dictionary types step 4.1.4.1 converts the member AT ITS
   OWN PLACE in the read order, and this performs no test the type has not already performed.
   WHAT THE MOVE FIXED IS AN ORDER A PAGE COULD SEE. The conversion used to be this body's, so it ran after the
   member loop had read EVERY member of the dictionary:
   `new MouseEvent("m", {relatedTarget: 42, get screenX(){ throw new Error("ran"); }})` reported `ran`, because
   `screenX` sorts after `relatedTarget` among MouseEventInit's OWN members and its step 4.1.3.1 Get ran the
   getter that a browser's step 4.1.4.1 TypeError had already made unreachable. It now reports the TypeError.
   AND NOT THE FocusEvent SPELLING, which an earlier statement of this gave and which distinguishes NOTHING —
   recorded here because it is the kind of claim a reader re-derives: §3.2.17 step 4 reads inherited
   dictionaries first, "in order from least to most derived", and only then each dictionary's own members
   lexicographically. `view` is declared on UIEventInit and `relatedTarget` on FocusEventInit, so `view` is read
   FIRST and a browser runs that getter exactly as this engine does; the two do not sort "at the same level",
   which core/events/focus_event.c's own constructor comment states in the file the caller lived in.
   FocusEventInit has no member after `relatedTarget` at all, so its order is not observable through a later
   member's getter and MouseEventInit's is the only spelling that shows this.
   ITS ONE REMAINING JUDGEMENT IS THE ABSENT DICTIONARY, and that is a POSITIVE statement rather than a hole a
   `?:` fills: §3.2.17 step 4.1.2's "If jsDict is either undefined or null" arm gives every member the value
   undefined, and DOM §2.2's un-initialized value of the associated relatedTarget is null — the same null
   `EventTarget? relatedTarget = null` places at step 4.1.5 for a page that passed a dictionary and left the
   member out. One fact, so the engine's own events and a page's constructed ones agree without a second table
   of defaults. */
JSValue event_target_nullable_of_dict(JSContext *ctx, JSValueConst init, const char *member)
{
    JSValue v;

    DCHECK(member != NULL && *member,
           "an `EventTarget?` member was read off a converted dictionary with no member name — the name is "
           "what identifies the declaration a wrong shape would have to be fixed at");
    v = idl_dict_get(ctx, init, member);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return JS_NULL;
    }
    /* THE BRAND REFUSED A WRONG VALUE BEFORE ANY BODY WAS ENTERED AND DOES NOT SEE AN UNKNOWN ONE — the same
       reading input_device_capabilities_of_dict makes of the same arm: §3.2.15's test is
       IDL_INTERFACE_NULLABLE's, and the §3.2.17 member loop rewrites a CONCOLIC member's type to IDL_ANY
       before that arm is asked, so `{relatedTarget: <unknown>}` is the one shape that reaches here having
       passed no brand at all, and it is the shape a message about a wrong value misnames. */
    IDL_DCHECK_MEMBER(JS_IsNull(v) || event_target_is_value(ctx, v), v, member,
                      "`EventTarget?` with a `= null` default — UI Events §3.3.1.2 FocusEventInit and Pointer "
                      "Events 4 §11.1 MouseEvent interface each write `EventTarget? relatedTarget = null` — "
                      "branded per member by IdlDictMember::iface_is over IDL_INTERFACE_NULLABLE");
    return v;
}

/* §2.9 steps 6.1-6.2 and 6.9.3-6.9.4: a NEW LIST holding each of the event's touch targets RETARGETED against
   `against`. JS_NULL when the event's touch target list is empty, which is every event but a TouchEvent and is
   the same list one allocation cheaper — the item's field carries that spelling too. OWNED. */
static JSValue dispatch_retarget_touch_targets(JSContext *ctx, JSValueConst ev, JSValueConst against)
{
    JSValue list = event_touch_target_list(ctx, ev), out;
    uint32_t i, n;

    if (!JS_IsArray(list)) {
        JS_FreeValue(ctx, list);
        return JS_NULL;
    }
    n = arr_len(ctx, list);
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "§2.9's retargeted touch target list could not be allocated");
    for (i = 0; i < n; i++) {
        JSValue t = JS_GetPropertyUint32(ctx, list, i);

        JS_SetPropertyUint32(ctx, out, i, event_target_retarget(ctx, t, against));
        JS_FreeValue(ctx, t);
    }
    JS_FreeValue(ctx, list);
    return out;
}

/* Step 6.9.6's second disjunct: "target's root is a shadow-including inclusive ancestor of parent". FALSE when
   `target` is not a node, which is the answer that matters — it is what stops the walk treating the window as a
   boundary crossing on the way past a detached target. */
static bool dispatch_target_root_contains(JSContext *ctx, JSValueConst target, JSValueConst parent)
{
    JSValue root;
    bool contains;

    if (!g_tree)
        return false;
    root = g_tree->root(ctx, target);
    contains = g_tree->is_shadow_including_inclusive_ancestor(ctx, root, parent);
    JS_FreeValue(ctx, root);
    return contains;
}

/* HTML §6.5 Activation behavior of elements' `click()` step 4: "Fire a synthetic pointer event named click at
   this element, with the not trusted flag set" — HTML §8.1.8.3 Event firing's steps 1-8, whose step 9 is the
   dispatch this machine already is.
   IT IS BUILT HERE AND NOT AT THE DISPATCH'S ENTRY BECAUSE STEP 7 NEEDS THE TARGET. `view` is "target's node
   document's Window object, if any, and null otherwise", which is §8.1.8.1's OWN step 4 term — the same fact
   about the same node, so it is asked through the same registered term rather than re-derived here, and a
   second derivation could disagree with it about which document a node belongs to.
   NULL IS THE STEP'S OWN ANSWER AND NOT A DEFAULT FILLING A HOLE — "if any, and null otherwise" — and a host
   that registered no terms is exactly the "otherwise": the terms come from whichever component owns the tree,
   so a host without them has no node documents at all and therefore no Window for one to have. That is the
   same reading handler_determine_target makes of the same null at step 1, and it is why this is a positive
   statement rather than an `if` past a broken invariant. What must not be guessed is the SHAPE of the answer,
   and mouse_event_new_synthetic asserts that: a Window or null, never anything else. */
static JSValue dispatch_synthetic_click(JSContext *ctx, JSValueConst target)
{
    /* BORROWED — the realm owns its global, exactly as §8.1.8.1 step 4's reader treats it. */
    JSValueConst view = g_handler_terms == NULL ? JS_NULL
                                                : g_handler_terms->node_document_global(ctx, target);

    return mouse_event_new_synthetic(ctx, "click", view);
}

static int js_dispatch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDispatchState *s = st;
    /* BOTH ARE JUMPED PAST. The re-entry gotos land after these are assigned on the straight path, so they are
       initialised at their declaration rather than left for a `goto` to read. */
    JSValue fn = JS_UNDEFINED, ignored = JS_UNDEFINED;
    int r;

    if (s->hdr.stage == DISPATCH_INIT) {
        JSValueConst target;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->path = s->type = s->lcb = s->exc = s->cur = s->act = s->arr = s->ev = s->result = JS_UNDEFINED;
        s->tgt = s->slottable = s->click_el = JS_UNDEFINED;
        {
            int k;
            STEP_CB_FOREACH(s->cb, k)
                s->cb[k] = JS_UNDEFINED;
        }
        report_exception_work_start(&s->rep);
        event_handler_work_start(&s->eh);
        s->ehc = 0;   /* §8.1.8.1 step 3's compile: nothing in flight — see the field */
        s->eh_index = -1;
        /* THREE ENTRIES, ONE MACHINE, and `arg` is decided at REGISTRATION so no call site chooses:
             DISPATCH_ARG  — dispatchEvent: the receiver is the target, the page supplied the event.
             CLICK_SYNTH   — HTML §6.5 Activation behavior of elements' click(): the receiver is the target and
                             the event is BUILT, because that method's step 4 is "Fire a synthetic pointer
                             event named click at this element, with the not trusted flag set", and HTML
                             §8.1.8.3 Event firing's LAST step is "Return the result of dispatching event at
                             target" — which IS this dispatch. (The number here used to be §3.2.2, which is
                             "Elements in the DOM" and defines no method at all.) THIS ENTRY IS THE WHOLE
                             METHOD AND NOT ONLY ITS STEP 4: steps 1-3 run just below, step 5 in the teardown.
             DISPATCH_PAIR — the ENGINE firing its own event, where there is no receiver to be the target
                             because the caller is C: both come in as arguments.
           The third is what let the second DELIVERY go. The engine used to enqueue each listener as its own
           job — a walk with no continuation, so it could not see stopImmediatePropagation, could not answer
           whether anything cancelled, and was a second implementation of §2.9 beside this one. */
        target = (s->hdr.arg == DISPATCH_PAIR) ? step_arg(&s->hdr, 0)
                                              : event_target_receiver(ctx, s->hdr.this_val);
        /* HTML §6.5 Activation behavior of elements' click() STEPS 1-3, which run BEFORE step 4 builds the
           event — this machine's CLICK_SYNTH entry IS that method, and step 4 is everything below.
           EACH OF THE THREE EXITS ANSWERS THE SAME THING, WHICH IS WHY THE RESULT MOVED. §6.5's steps return
           nothing and HTML's IDL declares `undefined click()`, so a click that returns at step 1 and one that
           runs the whole dispatch must be indistinguishable to the page. This machine answered `!canceled` for
           every entry, so adding steps 1-2 alone would have handed a page a boolean for a click that fired and
           `undefined` for one a disabled control refused — a way to read an element's disabled state that no
           browser exposes, invented by the guard that was supposed to model one. See the terminal below. */
        if (s->hdr.arg == CLICK_SYNTH) {
            /* STEP 1: "If this element is a form control that is disabled, then return."
               A HOST THAT REGISTERED NO PREDICATE HAS NO FORM CONTROLS, so nothing in it is one that is
               disabled — the same positive reading dispatch_get_parent takes of a host with no tree, and not a
               default filling a hole: HTML's form layer is what defines the concept, so a host without it has
               no elements the question is about rather than an unanswered question. */
            if (g_is_disabled_form_control != NULL && g_is_disabled_form_control(ctx, target))
                return JS_STEP_DONE;
            /* STEP 2: "If this element's click in progress flag is set, then return."
               THIS IS THE STEP THAT TERMINATES A RE-ENTRANT CLICK, and on this engine it is the ONLY thing
               that can: a handler whose body calls click() on its own element re-enters here, and §NO BOUNDS
               forbids answering that with a depth cap — the standard's own flag is the terminator, and it is
               per element rather than per stack, so `a.onclick = () => b.click()` and back still alternates
               exactly once each way, as it does in a browser. */
            if (click_in_progress(ctx, target))
                return JS_STEP_DONE;
            /* STEP 3: "Set this element's click in progress flag." The debt step 5 discharges is recorded in
               the SAME line that raises it, so there is no path that sets the flag without owing the unset. */
            click_in_progress_set(ctx, target, true);
            s->click_el = JS_DupValue(ctx, target);
        }
        s->ev = (s->hdr.arg == CLICK_SYNTH)
                    ? dispatch_synthetic_click(ctx, target)
                    : JS_DupValue(ctx, step_arg(&s->hdr, s->hdr.arg == DISPATCH_PAIR ? 1 : 0));
        /* ONLY CLICK_SYNTH CAN GET HERE WITH A LIVE THROW — the other two arms dup an argument. Its throw is
           PROPAGATED rather than falling into the brand check below, which would answer "not an Event" for an
           exception value and replace a real failure with a TypeError blaming the caller for an argument
           §8.1.8.3 built itself. */
        if (JS_IsException(s->ev))
            return JS_STEP_ABRUPT;
        /* NOT A DOM STEP AT ALL, WHICH IS WHY IT IS FIRST: DOM §2.7 Interface EventTarget declares
           `boolean dispatchEvent(Event event)`, so the argument is converted by Web IDL §3.2.15 Interface
           types — step 1 "If V implements I, then return the IDL interface type value…", step 2 "Throw a
           TypeError" — before any method step runs. The slot record is the brand a page cannot forge, so an
           object shaped like an event is still not one. */
        if (!event_is(ctx, s->ev)) {
            JS_ThrowTypeError(ctx, "dispatchEvent requires an Event");
            return JS_STEP_ABRUPT;
        }
        /* DOM §2.7 Interface EventTarget, the `dispatchEvent(event)` method steps, STEP 1: "If event's
           dispatch flag is set, OR if its initialized flag is not set, then throw an `InvalidStateError`
           DOMException." NOT a §2.9 step — §2.9's own step 1 is "Set event's dispatch flag", which is what
           the write below performs; a reader sent to §2.9 for this throw finds targetOverride.
           ONE SPEC STEP, TWO THROWS, DELIBERATELY. The step's two conditions are one sentence and one
           exception type, and splitting them costs nothing observable while the MESSAGE is the whole
           diagnostic value: which of the two conditions held is exactly what a reader needs, and a shared
           message would have described the second condition for a defect that was the first. */
        if (event_dispatch_flag(ctx, s->ev)) {
            JS_ThrowDOMException(ctx, "InvalidStateError", "the event is already being dispatched");
            return JS_STEP_ABRUPT;
        }
        /* The same step's other condition: an event whose INITIALIZED FLAG is unset cannot be dispatched.
           Only §4.5's createEvent makes one, and the two-call shape it exists for — `createEvent` then
           `initEvent` — is only meaningful because dispatching between them throws. */
        if (!event_initialized(ctx, s->ev)) {
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "the event was created by createEvent and never initialised");
            return JS_STEP_ABRUPT;
        }
        /* §2.9 dispatch step 1: "Set event's dispatch flag." */
        event_set_dispatch_flag(ctx, s->ev, true);
        /* §2.7's dispatchEvent(event) method step 2: "Initialize event's `isTrusted` attribute to false" —
           an event the PAGE dispatches is untrusted, whatever it was when constructed. One the ENGINE fires
           keeps the flag it was built with, which is the whole difference between them. (§2.9 step 3 is
           "Let activationTarget be null", which is `s->act`'s initialisation below.) */
        if (s->hdr.arg != DISPATCH_PAIR)
            event_set_trusted(ctx, s->ev, false);
        s->type = event_type(ctx, s->ev);
        s->clear_targets = 0;         /* step 5 */
        {
            /* §2.9 step 4: the event's relatedTarget RETARGETED against the target. Everything after it is
               about THIS value and not about the event's, which is the difference step 6 turns on. */
            JSValue er = event_related_target(ctx, s->ev);
            JSValue related = event_target_retarget(ctx, er, target);
            /* §2.9 step 6: "If target is not relatedTarget OR target is event's relatedTarget". When it fails,
               the whole of steps 6.1-6.14 is skipped — no path, no listeners, no activation target — and the
               dispatch runs steps 7-13 over an event that never propagated. That is what stops a `mouseover`
               whose relatedTarget retargets to the target itself from firing at it: the pointer never left the
               element as far as this element can tell. The second disjunct is the escape hatch for an event
               deliberately dispatched AT its own relatedTarget, which did not move and must still fire. */
            bool suppressed = same_target(target, related) && !same_target(target, er);
            JSValue touch = suppressed ? JS_NULL : dispatch_retarget_touch_targets(ctx, s->ev, target);

            JS_FreeValue(ctx, er);
            if (suppressed) {
                /* nothing to walk and nothing to invoke: both passes below find an empty path and fall through
                   to step 7. The event's own path stays the empty list, so composedPath answers with it. */
                s->tn = s->ti = s->n = s->i = 0;
                STEP_GOTO(s->hdr.stage, DISPATCH_CAPTURE, &s->aphase, &s->cphase, &s->hdr.get_phase, NULL);
            } else {
                /* §2.9 step 6.3: APPEND TO AN EVENT PATH with event, target, TARGETOVERRIDE, relatedTarget,
                   touchTargets and false. The path is the EVENT's — composedPath reads it — so it is published
                   on the event as it grows rather than kept privately here.
                   STEP 2: targetOverride is the target, UNLESS the dispatch was given one. HTML gives one for
                   `pagehide`, `pageshow`, `unload` and `beforeunload` — fired AT the Window with the DOCUMENT
                   as their target — through what the spec spells as the legacy target override flag. Without
                   it those events report the Window, which is what a page's `e.target` reads. */
                JSValueConst given = step_arg(&s->hdr, 2);
                JSValueConst override = JS_IsUndefined(given) ? target : given;

                s->path = event_path_new(ctx);
                event_path_append(ctx, s->path, target, override, related, touch,
                                  dispatch_is_closed_shadow_root(ctx, target), /*slotInClosedTree*/ false);
                /* THE SIZE IS THE PATH'S, never a counter kept beside it: the two are read together at every
                   step of the two passes, and a counter that drifts by one walks off the end or drops the
                   root. */
                s->tn = event_path_length(ctx, s->path);
                event_set_path(ctx, s->ev, s->path);
                /* §2.9 step 6.4: "Let isActivationEvent be true, if event is a MouseEvent object and event's
                   type attribute is `click`; otherwise false."
                   BOTH CONJUNCTS, AND THE BRAND IS THE ONE THAT DOES THE WORK. The type half alone made every
                   `new Event("click")` an activation event, and an activation event RUNS THE ACTIVATION
                   BEHAVIOUR at step 12.1 — it submits the form, follows the hyperlink, toggles the checkbox.
                   So a page that dispatched a plain Event named `click` navigated, which no browser does, and
                   every flow forked past that navigation was exploring a world that cannot happen.
                   THIS COMMENT USED TO SAY THE ENGINE HAD NO MouseEvent INTERFACE, and that was true when it
                   was written and became false the day core/events/mouse_event.c landed — a sentence that
                   stays right about the SPEC while going wrong about the TREE, which reads as authoritative
                   and is why it survived the interface arriving. `mouse_event_is` is the slot record and not
                   the class, so it stays true for PointerEvent, DragEvent and WheelEvent, which is what makes
                   it the brand step 6.4 asks for rather than a class comparison that would answer false for
                   the very interface a real click carries.
                   IT MEETS NO UNKNOWN. The brand is an OWN SLOT under a private Symbol read with
                   JS_GetOwnSlot, so it runs no page code and forks nothing: an object either carries the
                   record this engine wrote or it does not, and unknown external input carries no slots. The
                   type half is the string §2.2 stored at construction, likewise not a lookup. */
                s->is_activation = mouse_event_is(ctx, s->ev) && event_type_is(ctx, s->ev, "click");
                /* §2.9 step 6.5: the TARGET is the activation target if it has one — no `bubbles` condition
                   here, which is the difference from step 6.9.6.1's test on an ancestor. */
                if (s->is_activation && g_has_activation && g_has_activation(ctx, target))
                    s->act = JS_DupValue(ctx, target);
                /* step 6.6: "Let slottable be target, if target is a slottable and is ASSIGNED, and null
                   otherwise." An assigned slottable's get the parent answers with its SLOT, and step 6.9.1 is
                   the only place that can tell that hop apart from an ordinary parent — so the fact is carried
                   across the ask. */
                if (dispatch_is_assigned_slottable(ctx, target))
                    s->slottable = JS_DupValue(ctx, target);
                s->slot_in_closed_tree = 0;   /* step 6.7 */
                /* step 6.8: the first get the parent. `cur` carries the walk's frontier from here to the end of
                   6.9, and `tgt` carries the walk's own `target`, which 6.9.8.1 moves at every shadow
                   boundary. */
                s->cur = JS_DupValue(ctx, target);
                s->tgt = JS_DupValue(ctx, target);
                STEP_GOTO(s->hdr.stage, DISPATCH_PATH, &s->aphase, &s->cphase, &s->hdr.get_phase, NULL);
            }
            JS_FreeValue(ctx, related);
            JS_FreeValue(ctx, touch);
        }
    }

    if (s->hdr.stage == DISPATCH_PATH) {
        /* §2.9 step 6.9: "While parent is non-null" — append it and ask it for ITS parent. A tree is the PAGE's
           size, so this yields between parents rather than walking to the root inside one opcode; the frontier
           is on the state, so a park in the middle resumes at the ancestor it had reached. */
        JSValue parent;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        parent = dispatch_get_parent(ctx, s->cur, s->ev);
        JS_FreeValue(ctx, s->cur);
        s->cur = parent;
        if (JS_IsObject(parent)) {
            /* step 6.9.1: the parent the walk was just handed is the SLOT the previous item is assigned to —
               §4.4's get the parent says so, which is why this is an assert and not a test. Crossing INTO a
               closed shadow tree through a slot is what slot-in-closed-tree records, and it records it for
               THIS item, before the append below and until step 6.9.9 clears it. */
            if (JS_IsObject(s->slottable)) {
                DCHECK(dispatch_is_slot(ctx, parent),
                       "§2.9 step 6.9.1: the previous path entry is an ASSIGNED slottable, so §4.4's get the "
                       "parent must have answered with its assigned slot — a parent that is not a slot means "
                       "the two algorithms disagree about what `assigned` means");
                JS_FreeValue(ctx, s->slottable);
                s->slottable = JS_UNDEFINED;
                if (dispatch_root_is_shadow_root(ctx, parent, /*want_closed*/ true))
                    s->slot_in_closed_tree = 1;
            }
            /* step 6.9.2: and this parent may itself be slotted, one component inside another. */
            if (dispatch_is_assigned_slottable(ctx, parent))
                s->slottable = JS_DupValue(ctx, parent);
            /* steps 6.9.3-6.9.4: the event's relatedTarget and each of its touch targets, RETARGETED against
               THIS parent. They are per-ancestor values and not the dispatch's — the same relatedTarget is one
               object to an ancestor inside its shadow tree and the tree's host to one outside — which is why
               they are computed here, per iteration, and go into the item rather than onto the state. */
            {
                JSValue er = event_related_target(ctx, s->ev);
                JSValue related = event_target_retarget(ctx, er, parent);
                JSValue touch = dispatch_retarget_touch_targets(ctx, s->ev, parent);
                bool ended = false;

                JS_FreeValue(ctx, er);
                if (dispatch_is_window(ctx, parent) || dispatch_target_root_contains(ctx, s->tgt, parent)) {
                    /* step 6.9.6: still inside the tree the walk currently calls the target's, so the item gets
                       NO shadow-adjusted target and `invoke` will keep answering with the one further in.
                       6.9.6.1: an ANCESTOR becomes the activation target only for an event that BUBBLES, and
                       only while none has been picked — the nearest one, target first, wins. */
                    if (s->is_activation && !JS_IsObject(s->act) && event_bubbles(ctx, s->ev) &&
                        g_has_activation && g_has_activation(ctx, parent))
                        s->act = JS_DupValue(ctx, parent);
                    event_path_append(ctx, s->path, parent, JS_NULL, related, touch,
                                      dispatch_is_closed_shadow_root(ctx, parent), s->slot_in_closed_tree);
                } else if (same_target(parent, related)) {
                    /* step 6.9.7: the walk has reached the retargeted relatedTarget itself. "Set parent to
                       null" ENDS the walk without appending — the event never propagates past the object it is
                       reported as coming from, which is what makes `mouseout` stop at the common ancestor. */
                    ended = true;
                } else {
                    /* step 6.9.8: the walk has left the tree it was in — this parent is the shadow HOST — so
                       the event RETARGETS here: everything from this item outward reports the host as `target`,
                       which is the whole of what a shadow tree hides.
                       6.9.8.2 has NO `bubbles` condition, unlike 6.9.6.1 — the host of a shadow tree the event
                       came out of is an activation target for a non-bubbling event too. */
                    JS_FreeValue(ctx, s->tgt);
                    s->tgt = JS_DupValue(ctx, parent);
                    if (s->is_activation && !JS_IsObject(s->act) && g_has_activation &&
                        g_has_activation(ctx, parent))
                        s->act = JS_DupValue(ctx, parent);
                    event_path_append(ctx, s->path, parent, parent, related, touch,
                                      dispatch_is_closed_shadow_root(ctx, parent), s->slot_in_closed_tree);
                }
                JS_FreeValue(ctx, related);
                JS_FreeValue(ctx, touch);
                if (!ended) {
                    s->tn = event_path_length(ctx, s->path);
                    /* step 6.9.10 "Set slotInClosedTree to false", and it is per ITERATION, not per tree.
                       6.9.9 is the get the parent directly above it, which the fall-through below names. */
                    s->slot_in_closed_tree = 0;
                    return JS_STEP_YIELD;
                }
                /* step 6.9.7 set parent to null, so 6.9.9 does not ask again and the while ends: fall out of
                   the walk with the path exactly as it stands. */
            }
        }
        JS_FreeValue(ctx, s->cur);
        s->cur = JS_UNDEFINED;
        JS_FreeValue(ctx, s->slottable);
        s->slottable = JS_UNDEFINED;
        /* steps 6.10-6.11: clearTargetsItem is the LAST item with a non-null shadow-adjusted target — the
           outermost thing the event still calls its target — and if ANY of that item's three target-bearing
           fields is a node inside a shadow tree, all three are CLEARED once the walk is over (step 11), so a
           page holding the event afterwards cannot read a node out of a tree it was never given. It is a
           DISJUNCTION over shadow-adjusted target, relatedTarget and every entry of the touch target list:
           an event whose TARGET is in the document tree can still carry a relatedTarget that is not. */
        {
            uint32_t k = s->tn;

            while (k-- > 0) {
                JSValue item = event_path_item(ctx, s->path, k);
                JSValue sat = event_path_shadow_adjusted_target(ctx, item);
                bool found = JS_IsObject(sat);

                if (found) {
                    JSValue related = event_path_related_target(ctx, item);
                    JSValue touch = event_path_touch_targets(ctx, item);

                    s->clear_targets = dispatch_root_is_shadow_root(ctx, sat, /*want_closed*/ false) ||
                                       dispatch_root_is_shadow_root(ctx, related, /*want_closed*/ false);
                    if (!s->clear_targets && JS_IsArray(touch)) {
                        uint32_t j, m = arr_len(ctx, touch);

                        for (j = 0; j < m && !s->clear_targets; j++) {
                            JSValue t = JS_GetPropertyUint32(ctx, touch, j);

                            s->clear_targets = dispatch_root_is_shadow_root(ctx, t, /*want_closed*/ false);
                            JS_FreeValue(ctx, t);
                        }
                    }
                    JS_FreeValue(ctx, related);
                    JS_FreeValue(ctx, touch);
                }
                JS_FreeValue(ctx, sat);
                JS_FreeValue(ctx, item);
                if (found)
                    break;
            }
        }
        s->ti = 0;
        s->n = s->i = 0;
        STEP_GOTO(s->hdr.stage, DISPATCH_CAPTURE, &s->aphase, &s->cphase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == DISPATCH_ACTIVATION) {
        /* re-entered inside the activation behaviour — see below. It waits on the HOST, not on a call, so
           whatever the resume carries is not this machine's and is released here rather than leaked. */
        JS_FreeValue(ctx, cb_result);
        goto activation;
    }
    /* A re-entry with no call in flight carries nothing this walk wants; the one that does is consumed by the
       request it belongs to, at resume_listener. The handler algorithm's stage is one of the four things that
       can be in flight: its Web IDL return-type coercion and its step 6 fork both park with `cphase` back at
       zero, so a walk that asked only about the call would free the answer they are waiting for. */
    if (s->cphase == 0 && s->lphase == 0 && s->reporting == 0 && s->eh.stage == 0 && s->ehc == 0)
        { JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED; }
    /* §2.9 steps 6.13 and 6.14: TWO passes over the path, not three legs over parts of it. The old shape ran the
       target as a leg of its own with BOTH kinds of listener in registration order, which is a different answer
       from the spec's and one the corpus asks for directly: at the target a CAPTURING listener runs in the
       capturing pass and a bubbling one in the bubbling pass, so registering bubble-then-capture still fires
       capture first. It also made "does this event bubble" a question about the LEG rather than about the ITEM,
       and for the target item the answer is that it is invoked either way. */
    for (;;) {
        while (s->i < s->n) {
            JSValue rec;

            /* §2.9: stopImmediatePropagation ends the walk between listeners, which is the only place it can
               be observed — the flag was set by a listener that has already returned. */
            if (s->reporting)
                goto report_throw;   /* re-entered inside the REPORT of what the last listener threw */
            /* RE-ENTERED INSIDE §8.1.8.1 STEP 3'S COMPILE, and this is tested BEFORE the call-phase arm below
               because the compile BORROWS `cphase` — its request is a call like any other, so `cphase` alone
               cannot tell "the program that produces the handler" from "the handler". The record is not
               re-read for the same reason the resume below does not re-read it: what the walk needs across
               this park is on the state (`lcb` holds the uncompiled record, `eh_index` the attribute). */
            if (s->ehc != 0)
                goto compile_handler;
            if (s->cphase == 0 && s->lphase == 0 && s->eh.stage == 0 && s->ehc == 0
                && event_stop_immediate(ctx, s->ev))
                break;
            if (s->cphase != 0 || s->eh.stage != 0) {
                /* RE-ENTERED INSIDE THE CALL BELOW. The record is NOT re-read: `once` has already removed it
                   from the live list and set its removed field, so a re-read would skip the very listener whose
                   answer is arriving and lose the call's result. The callee is held by the request buffer. */
                fn = JS_UNDEFINED;
                goto resume_listener;
            }
            if (s->lphase != 0)
                goto resolve_operation;   /* re-entered inside the `handleEvent` READ — same reason */
            rec = JS_GetPropertyUint32(ctx, s->arr, s->i);
            if (!JS_IsObject(rec)) {
                JS_FreeValue(ctx, rec);
                s->i++;
                continue;
            }
            /* §2.9 "inner invoke" step 2: a listener whose REMOVED field is set is skipped. The walk iterates a
               SNAPSHOT so that an added listener does not run, and the spec's own note says removal must still
               have an effect — which is exactly what the field is for. Without it, `removeEventListener` called
               from inside a dispatch removed the listener from the live list and the snapshot ran it anyway. */
            if (rec_flag(ctx, rec, "removed")) {
                JS_FreeValue(ctx, rec);
                s->i++;
                continue;
            }
            /* §2.9 "inner invoke" steps 2.3-2.4: the CAPTURING pass runs only capturing listeners and the
               BUBBLING pass only the others, at every item of the path INCLUDING the target. */
            if (rec_flag(ctx, rec, "capture") != (s->hdr.stage == DISPATCH_CAPTURE)) {
                JS_FreeValue(ctx, rec);
                s->i++;
                continue;
            }
            fn = rec_cb(ctx, rec);
            /* §8.1.8.1: the HANDLER SLOT resolves HERE, at dispatch time, to whatever `ontype` currently is —
               that is what keeps the slot's POSITION in the list while its handler changes underneath it. An
               UNSET handler is not a listener at all, which is why this one skip stays a skip: there is nothing
               to look an operation up on. */
            s->eh_index = -1;
            if (JS_VALUE_GET_PTR(fn) == JS_VALUE_GET_PTR(g_handler_marker)) {
                const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
                JS_FreeValue(ctx, fn);
                /* HTML §8.1.8.1 steps 2-3: GETTING THE CURRENT VALUE of the event handler, and returning when
                   it is null. It is performed here rather than inside the algorithm because §2.9's inner
                   invoke needs the same answer for its own reason — a handler slot whose handler is null is
                   not a listener at all and the walk skips it without invoking anything — and one question
                   asked twice is two answers. */
                fn = t ? handler_current(ctx, s->cur, t) : JS_UNDEFINED;
                s->eh_index = t ? (int16_t)eh_index_of_type(t) : -1;
                if (t) JS_FreeCString(ctx, t);
                /* NULL AND NOT-CALLABLE ARE DIFFERENT ANSWERS HERE. Step 3's null is this skip; a non-callable
                   OBJECT is a handler the page assigned and Web IDL kept, and its TypeError belongs to §3.12's
                   invoke inside the algorithm — where §2.9 inner invoke step 2.11 reports it and the walk
                   carries on, which is what a browser does and what skipping it silently would not. */
                if (!JS_IsObject(fn)) {
                    JS_FreeValue(ctx, fn);
                    JS_FreeValue(ctx, rec);
                    s->i++;
                    continue;
                }
                /* A HANDLER RAN FOR A TYPE THE ATTRIBUTE LIST DOES NOT NAME. The marker is placed by
                   js_handler_set and by nothing else, and that setter is reached only through an accessor the
                   list itself installs — so a resolved marker whose type is absent from the list means the
                   two have come apart, and §8.1.8.1 would have no declared Web IDL callback type to select. */
                DCHECK(s->eh_index >= 0,
                       "an event handler slot resolved for an event type HTML §8.1.8.2's tables do not name — "
                       "the marker and the attribute list are placed by one mechanism and have disagreed");
            }
            /* §2.9 "inner invoke" step 2.5: a `once` listener is removed BEFORE it is called, so a listener that
               re-enters this dispatch cannot see itself. Removing it after would let a re-entrant fire run it
               a second time, which is the exact bug `once` exists to prevent. */
            if (rec_flag(ctx, rec, "once")) {
                const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
                if (t) { listener_remove_record(ctx, s->cur, t, rec); JS_FreeCString(ctx, t); }
            }
            /* §2.9 "inner invoke" step 2.9: a PASSIVE listener raises the event's in-passive listener flag for
               the duration of the call, which is what makes its preventDefault() do nothing. */
            s->in_passive = rec_flag(ctx, rec, "passive");
            if (s->in_passive)
                event_set_in_passive(ctx, s->ev, true);
            JS_FreeValue(ctx, rec);
            JS_FreeValue(ctx, s->lcb);
            s->lcb = fn;   /* held across the operation lookup below, which can run the page's code */
            /* AN EVENT HANDLER IS A CALLBACK FUNCTION TYPE AND NOT A CALLBACK INTERFACE, so the operation
               lookup below is not its algorithm: §8.1.8.1 step 5 invokes the callback ITSELF, and a
               non-callable one is a TypeError at Web IDL §3.12's invoke rather than an object with a
               `handleEvent` to find. Reading one off `el.onclick = {handleEvent(){…}}` would run a method no
               browser runs, and would do it while reporting nothing. */
compile_handler:
            DCHECK(s->ehc == 0 || s->eh_index >= 0,
                   "§8.1.8.1 step 3's compile is in flight for a slot the walk does not call an event handler "
                   "— `ehc` is raised only inside the arm `eh_index` selects, so the two have come apart and "
                   "the resume would fall through to the callback-interface lookup with the compile still up");
            if (s->eh_index >= 0) {
                /* HTML §8.1.8.1's GET THE CURRENT VALUE steps 3.8-3.12. handler_current stopped at the last
                   step that runs no code and answered the INTERNAL RAW UNCOMPILED HANDLER when the compile is
                   owed; this is one of the TWO drivers that finish it, because a program evaluation needs a
                   JSStepHdr and a request buffer and this machine has both.
                   THE OTHER DRIVER IS THE IDL ATTRIBUTE GETTER, and saying so here is the point: this comment
                   used to justify itself by saying the plain C accessor does not have them — unquoted, because
                   it is this file's own retired sentence and not a standard's — and that was a true statement
                   about a getter which has since become a machine of its own. Both call the SAME
                   handler_compile_run over their own stage byte, phase and buffer, so §8.1.8.1 step 3 has one
                   implementation and not one per door. It happens ONCE per handler either way — step 3.12
                   writes the function back to the map — so whichever door arrives second finds a function and
                   this whole block is a predicate that answers false. */
                if (s->ehc != 0 || handler_compile_owed(ctx, s->lcb)) {
                    JSValue compiled = JS_UNDEFINED;
                    const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;

                    DCHECK(t != NULL,
                           "§8.1.8.1 step 3's compile was reached for a dispatch whose event type is not a "
                           "string — the walk resolved this slot BY that type one step earlier");
                    r = handler_compile_run(ctx, &s->ehc, &s->cphase, STEP_CB(s->cb), s->cur,
                                            t ? t : "", cb_result, &compiled, out_cb, out_argc);
                    if (t) JS_FreeCString(ctx, t);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked evaluating the program; the resume comes back here */
                    if (r < 0) {
                        /* The compile was abrupt. §2.9 "inner invoke" step 2.11's arm is the same one a
                           listener's own throw takes — REPORT it and carry on down the listener list — which
                           is what a browser does with a handler it could not produce, and is the only arm
                           that does not silently drop a slot the page can see is registered. */
                        s->ehc = 0;
                        goto listener_threw;
                    }
                    JS_FreeValue(ctx, s->lcb);
                    s->lcb = compiled;   /* step 3.12's value, which the map now holds too */
                }
                fn = JS_DupValue(ctx, s->lcb);
                goto resume_listener;
            }
resolve_operation:
            /* §2.9 "inner invoke" step 2.11 is "CALL A USER OBJECT'S OPERATION with listener's callback and
               `handleEvent`", and Web IDL §3.11 "Callback interfaces" step 10 is what that means for a callback
               INTERFACE: a callable callback is itself the operation and keeps the given `this`; a NON-callable
               one has `handleEvent`
               READ OFF IT — per invocation, so a page that swaps the method between two dispatches gets both —
               and is then the `this` of that call.
               The read is the page's code (an accessor, a Proxy trap), so it is a REQUEST and not a
               JS_GetPropertyStr; that is also why `event-global-set-before-handleEvent-lookup` can observe where
               in the algorithm it happens. */
            fn = JS_DupValue(ctx, s->lcb);
            if (!JS_IsFunction(ctx, fn)) {
                JSAtom op;
                JSValue m = JS_UNDEFINED;

                JS_FreeValue(ctx, fn);
                op = JS_NewAtom(ctx, "handleEvent");
                r = step_getprop_run(ctx, &s->hdr, s->lcb, op, cb_result, &m, out_cb, out_argc);
                JS_FreeAtom(ctx, op);
                cb_result = JS_UNDEFINED;
                if (r > 0) { s->lphase = 1; return r; }   /* parked ON THE READ; the resume comes back here */
                s->lphase = 0;
                if (r < 0) {
                    /* Web IDL §3.11 step 10.2: an ABRUPT Get is RETURNED as it stands. The read reports it here
                       because this machine's definition declares catches_abrupt, and §2.9 "inner invoke" step
                       2.11 says what to do with it — REPORT it and carry on down the listener list, never
                       unwind the dispatch. `EventListener-handleEvent`'s "rethrows errors when getting
                       handleEvent" is exactly this listener. */
                    goto listener_threw;
                }
                if (!JS_IsFunction(ctx, m)) {
                    /* Web IDL §3.11 step 10.4: a non-callable operation is a TypeError, reported the same way. */
                    JS_ThrowTypeError(ctx, "the event listener's `handleEvent` is not callable");
                    JS_FreeValue(ctx, m);
                    goto listener_threw;
                }
                fn = m;
                /* Web IDL §3.11 step 10.5: the receiver becomes the callback OBJECT, overriding currentTarget. */
                r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, s->lcb, 1, (JSValueConst *)&s->ev,
                                  cb_result, &ignored, out_cb, out_argc);
                JS_FreeValue(ctx, fn);
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;
                goto listener_returned;
            }
            /* §2.9 "inner invoke" step 2.11: the listener is called with `this` = currentTarget and the event as
               its one argument. A CALL REQUEST, so the listener is ordinary preemptible page code and this
               machine parks — which is the whole reason the engine's own firing can share this walk. */
resume_listener:
            /* AN EVENT HANDLER IS NOT DELIVERED LIKE A LISTENER — HTML §8.1.8.1's processing algorithm steps
               4-6 are what §8.1.8.1's activate an event handler registered this slot to run. It decides the ARGUMENT LIST (five for an
               `OnErrorEventHandler` at a global, one otherwise) and it READS THE RETURN VALUE, which is the
               half DOM §2.9 discards for a plain listener and must go on discarding. Everything else about
               this listener — the snapshot, the `once` removal, the passive flag, the report of a throw — is
               the same walk, which is why this is one branch and not a second delivery path. */
            if (s->eh_index >= 0) {
                r = event_handler_run(ctx, &s->eh, &s->hdr, &s->cphase, STEP_CB(s->cb), fn, s->ev,
                                      EH_NAME[s->eh_index], cb_result, out_cb, out_argc);
                JS_FreeValue(ctx, fn);
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;     /* parked in the handler's body, its coercion, or its step 6 fork */
                if (r < 0) {
                    /* §8.1.8.1 step 5 invokes with "rethrow", and §2.9 inner invoke step 2.11 is where that
                       lands: report it and carry on down the listener list, exactly as for a listener. */
                    goto listener_threw;
                }
                goto listener_returned;  /* the algorithm consumed the return value; there is no `ignored` */
            }
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, s->cur, 1, (JSValueConst *)&s->ev,
                              cb_result, &ignored, out_cb, out_argc);
            JS_FreeValue(ctx, fn);   /* the request DUP'd it into the buffer, which is what holds it parked */
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;         /* parked ON THIS LISTENER; the resume comes back to it */
listener_returned:
            if (s->in_passive) {
                event_set_in_passive(ctx, s->ev, false);
                s->in_passive = 0;
            }
            JS_FreeValue(ctx, s->lcb);
            s->lcb = JS_UNDEFINED;
            /* §2.9 "inner invoke" step 2.11: "If this throws an exception exception: report exception". THE
               WALK DOES NOT UNWIND — the next listener runs, the remaining path items run, and dispatchEvent
               still answers !canceled. This machine therefore DECLARES catches_abrupt, so a throwing listener
               arrives as a value here instead of tearing the dispatch down; without it, one page's throw
               skipped every listener after it and the exception was swallowed with nothing to say so. */
            if (JS_IsException(ignored)) {
                ignored = JS_UNDEFINED;
listener_threw:
                /* Reached from the operation lookup as well, which has not run the cleanup above — both are
                   idempotent, which is why one label serves both arrivals. */
                if (s->in_passive) { event_set_in_passive(ctx, s->ev, false); s->in_passive = 0; }
                JS_FreeValue(ctx, s->lcb);
                s->lcb = JS_UNDEFINED;
                /* §2.9 "inner invoke" step 2.11's OTHER half: "set legacyOutputDidListenersThrowFlag". It is
                   an output of the dispatch and it survives it — see event.h. Indexed Database §5.9/§5.10
                   read it to decide whether the transaction rolls back. */
                event_set_listeners_threw(ctx, s->ev, true);
                s->exc = JS_GetException(ctx);
                s->reporting = 1;
            }
            JS_FreeValue(ctx, ignored);  /* §2.9: a listener's return value is discarded */
report_throw:
            if (s->reporting) {
                r = report_exception_run(ctx, &s->rep, s->exc, cb_result, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;    /* parked inside the `error` event's own dispatch */
                s->reporting = 0;
                JS_FreeValue(ctx, s->exc);
                s->exc = JS_UNDEFINED;
            }
            s->i++;
        }
        /* ON TO THE NEXT PATH ITEM, and then to the next pass. `ti` names the item ABOUT to be invoked and is
           advanced once its listeners are set up, so the FIRST item of each pass is ti == 0 and no entry point
           has to say so twice. */
        for (;;) {
            uint32_t idx;
            const char *type;
            bool at_target;
            JSValue item, sat;

            if (s->ti >= s->tn) {
                if (s->hdr.stage == DISPATCH_BUBBLE) goto walked;
                STEP_GOTO(s->hdr.stage, DISPATCH_BUBBLE, &s->aphase, &s->cphase, &s->hdr.get_phase, NULL);
                s->ti = 0;
                continue;
            }
            /* the capturing pass walks the path in REVERSE (root first); the bubbling pass walks it forwards. */
            idx = (s->hdr.stage == DISPATCH_CAPTURE) ? (s->tn - 1 - s->ti) : s->ti;
            item = event_path_item(ctx, s->path, idx);
            sat = event_path_shadow_adjusted_target(ctx, item);
            /* §2.9 steps 6.13.1 / 6.14.1: AT_TARGET is not "index zero", it is "this item HAS a shadow-adjusted
               target" — which is true of every item the event retargeted at, so an event dispatched inside a
               shadow tree is AT_TARGET twice, once for the node and once for its host. */
            at_target = JS_IsObject(sat);
            /* §2.9 step 6.14.2.1: in the BUBBLING pass an item that is not the target is skipped entirely for an
               event that does not bubble. The TARGET is invoked either way — which is why a non-bubbling event
               still reaches the target's non-capturing listeners. */
            if (!at_target && s->hdr.stage == DISPATCH_BUBBLE && !event_bubbles(ctx, s->ev)) {
                JS_FreeValue(ctx, sat);
                JS_FreeValue(ctx, item);
                s->ti++;
                continue;
            }
            /* §2.9 steps 6.13.1-6.13.2: the phase is AT_TARGET for an item that is a target, whichever pass is
               running, and the pass's own phase otherwise. */
            event_set_phase(ctx, s->ev, at_target ? 2 : (s->hdr.stage == DISPATCH_CAPTURE ? 1 : 3));
            /* "invoke" steps 1-3: the event's TARGET is the nearest shadow-adjusted target AT OR BEFORE this
               item — so every entry inside a shadow tree reports the node the event was dispatched at, and
               every entry from the host outward reports the host. It is a walk BACKWARD along the path and not
               a value set once for the dispatch, which is the whole of what retargeting is. */
            {
                uint32_t k = idx;

                while (!JS_IsObject(sat) && k > 0) {
                    JSValue back;

                    JS_FreeValue(ctx, sat);
                    back = event_path_item(ctx, s->path, --k);
                    sat = event_path_shadow_adjusted_target(ctx, back);
                    JS_FreeValue(ctx, back);
                }
                DCHECK(JS_IsObject(sat),
                       "§2.9 invoke step 2 ran off the front of the event path — item 0 is appended at step 6.3 "
                       "with the target as its shadow-adjusted target, so the backward walk always stops");
            }
            event_set_target(ctx, s->ev, sat);
            JS_FreeValue(ctx, sat);
            /* "invoke" steps 4-5: the event's relatedTarget and touch target list are THIS ITEM's — the forms
               §2.9 retargeted against this item's invocation target while it built the path. They are set per
               item and not once for the walk, for the same reason `target` is: a listener outside a shadow tree
               must read the host where one inside reads the node. */
            {
                JSValue related = event_path_related_target(ctx, item);
                JSValue touch = event_path_touch_targets(ctx, item);

                event_set_related_target(ctx, s->ev, related);
                event_set_touch_target_list(ctx, s->ev, touch);
                JS_FreeValue(ctx, related);
                JS_FreeValue(ctx, touch);
            }
            /* "invoke" step 6: a walk that has been stopped still RUNS, item by item, and returns before it
               invokes anything. That is not the same as ending the walk here, which is what this did: the
               event's `target` is written by steps 1-3 above BEFORE the return, so the outer entries of a
               retargeted path go on adjusting it after a listener has called stopPropagation. */
            if (event_stop_propagation(ctx, s->ev)) {
                JS_FreeValue(ctx, item);
                s->ti++;
                continue;
            }
            JS_FreeValue(ctx, s->cur);
            JS_FreeValue(ctx, s->arr);
            s->cur = event_path_invocation_target(ctx, item);   /* "invoke" step 7 */
            JS_FreeValue(ctx, item);
            event_set_current(ctx, s->ev, s->cur);
            type = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
            s->arr = type ? listener_snapshot(ctx, s->cur, type) : JS_NewArray(ctx);
            if (type) JS_FreeCString(ctx, type);
            s->n = arr_len(ctx, s->arr);
            s->i = 0;
            s->ti++;
            break;
        }
    }

walked:
    JS_FreeValue(ctx, cb_result);
    /* §2.9 steps 7-10: eventPhase NONE, currentTarget null, the path empty, and the dispatch and both stop
       flags UNSET — one operation, because the spec states them together and because leaving the stop flags set
       made the SAME event unusable for a second dispatch. */
    event_end_dispatch(ctx, s->ev);
    /* §2.9 step 11: if clearTargets, the event's target, relatedTarget and touch target list are CLEARED —
       ONE operation, because the standard states them as one step and because two of the three shipped without
       the first. See event.h. */
    if (s->clear_targets)
        event_clear_targets(ctx, s->ev);

activation:
    /* §2.9's step 12, and it is last for a reason: the activation behaviour runs AFTER the whole walk and
       ONLY if nothing cancelled — which is the entire meaning of `preventDefault()` on a click. It runs with
       the event already cleaned up, so a behaviour that reads `currentTarget` sees null, as it must. */
    if (JS_IsObject(s->act) && !event_canceled(ctx, s->ev)) {
        int ar;
        DCHECK(g_run_activation != NULL, "an activation target was picked with nothing to perform");
        /* STAGE 2 IS THE RESUME POINT. The behaviour may wait on the host, and when it does the whole dispatch
           parks here — after the walk, with the event already cleaned up — and re-enters at exactly this line
           rather than replaying three legs of listeners. */
        STEP_GOTO(s->hdr.stage, DISPATCH_ACTIVATION, &s->aphase, &s->cphase, &s->hdr.get_phase, NULL);
        ar = g_run_activation(ctx, s->act, s->ev, &s->aphase, &s->areq);
        if (ar != JS_STEP_DONE) return ar;
    }
    /* DOM §2.9 dispatch's last step, "Return false if event's canceled flag is set; otherwise true" — which
       DOM §2.7's `dispatchEvent(event)` step 3 returns, and which HTML §6.5's `click()` DOES NOT: its step 4
       fires the event and its step 5 is the last thing it does, so the method has no return value at all and
       HTML's IDL says so (`undefined click()`). The dispatch still runs in full and its canceled flag still
       decides the activation behaviour above; what differs is only what the METHOD hands back. Answering the
       boolean here made `el.click()` evaluate to true or false in a page where every browser answers undefined,
       and — once steps 1 and 2 existed — would have made the two early returns distinguishable from the fire.
       DISPATCH_PAIR is the engine's own C door and reads the boolean the same way dispatchEvent does. */
    s->result = (s->hdr.arg == CLICK_SYNTH) ? JS_UNDEFINED
                                            : JS_NewBool(ctx, !event_canceled(ctx, s->ev));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_dispatch_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_ARG, .catches_abrupt = 1, .visit = js_dispatch_visit,
    .algorithm = "DOM §2.9 dispatch", .steps = DISPATCH_STEPS
};
/* HTML §6.5 Activation behavior of elements' click(). The SAME machine — the method's step 4 IS a dispatch, and
   giving it its own would be two implementations of §2.9 that could disagree about listener order, the handler
   slot or the canceled flag. Its OTHER four steps are the CLICK_SYNTH arms above and below: steps 1-3 at the
   machine's entry and step 5 in its teardown. (The number here used to be §3.2.2, which is "Elements in the
   DOM" and defines no method at all.) */
static const JSTrampStepDef js_click_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, CLICK_SYNTH, .catches_abrupt = 1, .visit = js_dispatch_visit,
    .algorithm = "DOM §2.9 dispatch", .steps = DISPATCH_STEPS
};

void event_target_install_click(JSContext *ctx, JSValueConst target)
{
    DCHECK(JS_IsObject(target), "click was installed on something that is not an object");
    if (g_click_stepid < 0)
        g_click_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_click_def);
    idl_install_step_method(ctx, target, "click", 0, g_click_stepid);
}

/* THE INTERNAL DOOR, MINTED IN THE FIRING REALM. A C function's `ctx` is its DEFINING realm, and this machine
   reads §7.6's window off it — so the dispatcher a child document fires through has to be the child's. It costs
   one function object per fire, which a dispatch that already snapshots a listener list per target does not
   notice, and it removes the runtime-lifetime object that would otherwise have to be freed and per-realm at the
   same time. OWNED by the caller. */
static JSValue dispatch_fn_new(JSContext *ctx)
{
    JSValue fn;

    DCHECK(g_dispatch_pair_stepid >= 0,
           "the engine fired an event before event_target_init declared the dispatcher — there is one dispatch, "
           "and this is the only way a C caller reaches it");
    fn = JS_NewCFunction2(ctx, NULL, "dispatch", 2, JS_CFUNC_step, g_dispatch_pair_stepid);
    CHECK(!JS_IsException(fn), "the internal event dispatcher could not be allocated");
    return fn;
}

/* THE ENGINE FIRING ITS OWN EVENT ON A TASK SOURCE — the reach for a caller whose standard says "QUEUE a task
   … to fire an event named X": HTML §13.2.7 "The end" for `DOMContentLoaded` and `load`, §4.11.4 "The dialog
   element" for `close`, §4.10.5.4 "Common input element APIs" for `cancel`, §7.4.6.2 "Updating the document"
   for `hashchange`, Permissions §6.3.4 "onchange attribute" for `change`. It builds the event and hands it to
   the SAME §2.9 machine event_target_fire_run reaches; what differs between the two is WHICH of HTML §8.1.7
   "Event loops" queues the dispatch lands on, and that is the standard's choice at the call site rather than
   this component's.
   IT WAS A MICROTASK, AND HTML §8.1.7.3 "Processing model" IS WHY THAT IS NOT A SMALLER VERSION OF THE SAME
   THING: the microtask checkpoint drains BEFORE the next task begins, so every fire queued here — the whole
   document lifecycle among them — ran AHEAD of every task already standing: an expired `setTimeout(f,0)`, a
   delivered `postMessage`, a queued navigation. quickjs.h's own contract states the rule ("choosing the wrong
   one is not a performance detail: it reorders what the page observes") and this file chose the wrong one for
   every engine-initiated dispatch there is.
   THERE IS NO THIRD REACH, and that is what stops the choice being made again: no standard queues a MICROTASK
   to fire an event, so a caller is either one HTML queues a task for (this) or one whose fire is a bare
   synchronous step of an algorithm already running (event_target_fire_run, below). A caller that cannot park
   and whose fire is synchronous is not a third case — it is a caller that is not yet a step machine.
   That is the whole fix: this used to walk the listener list ITSELF and enqueue each listener as
   its own job, which was a second implementation of §2.9 beside the machine — one that could not see
   stopImmediatePropagation (each listener was a separate job with no walk between them), could not bubble
   properly (the caller passed the window in by hand as `bubble_to`), and could not answer whether anything
   cancelled. There is one dispatch now.
   The event stays TRUSTED, which is what distinguishes one the engine fired from one the page dispatched. */
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev, JSValueConst target_override)
{
    JSValueConst argv[3];
    JSValue fn;

    /* §2.9 dispatches AT an event target, and a queued fire is the one reach whose caller has already returned
       by the time the dispatch runs — so a target that is not an object arrives at the machine with nothing
       left to name the caller that supplied it. Asked here, at the enqueue, which is that caller's own line. */
    DCHECK(JS_IsObject(target), "an engine fire was queued at something that is not an event target");
    if (JS_IsException(ev)) { JS_FreeValue(ctx, ev); return; }
    argv[0] = target;
    argv[1] = ev;
    /* §2.9 STEP 2's targetOverride, AS AN ARGUMENT. It is a parameter OF THE DISPATCH and not state on the
       event — the same event fired twice, once with an override and once without, is two dispatches with two
       different `target`s — so it travels with the invocation.
       IT IS THE TARGET ITSELF rather than HTML's boolean, because the flag's whole content is "use the target's
       associated Document", and the caller that passes it is the one holding that Document. Asked as a boolean,
       this component would have to reach into document.c to resolve a Window it may not even own the realm of;
       asked as the value, it is the spec's own parameter and there is nothing to resolve.
       There was no way to pass it at all before, so `pagehide`, `pageshow`, `unload` and `beforeunload` — the
       only fires HTML gives it to — would have reported the Window where the spec says the Document. */
    argv[2] = target_override;
    fn = dispatch_fn_new(ctx);
    /* A TASK, on HTML §8.1.7.2 "Queuing tasks"' half of the event loop — the queue the callers' standards name.
       It is still a call-root flow, so it is preemptible, forkable and parkable like any other program, which
       is what every listener body needs and what a C activation cannot host; the queue decides only WHEN the
       event loop begins it relative to the microtasks and tasks already outstanding. */
    JS_EnqueueCallTask(ctx, fn, 3, argv);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ev);
}

/* THE SAME FIRE, SYNCHRONOUSLY — for a caller that CAN park. §2.9 dispatch is synchronous, and some of the
   engine's own fires are specified that way: §3.2's `abort` happens inside abort(), so a page that calls
   ac.abort() and then reads a flag its listener set must see it already set. A queued fire answers the
   question after the caller has returned.
   It is the SAME machine through the same internal door — only the reach differs, which is the whole point of
   there being one dispatch: a caller that can park calls it as a REQUEST, one that cannot enqueues it as a job.
   `phase` and `cb` are the caller machine's own; `cb` needs FOUR slots ([this, func, target, event]), and it
   is FORWARDED, so its capacity is forwarded with it and the caller passes both through STEP_CB — a buffer
   that has decayed to a pointer can no longer say how big it is.
     0 = done (*pnot_canceled set when asked), 3 = the caller must return that step code. */
int event_target_fire_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst target,
                          JSValueConst ev, JSValueConst target_override, JSValue in,
                          bool *pnot_canceled, JSValue **out_cb, int *out_argc)
{
    JSValueConst argv[3];
    JSValue out = JS_UNDEFINED;
    int r;

    /* ASKED ON BOTH LEGS, because the resume leg forwards the same capacity and a caller that got the first one
       right by accident must not get the second one wrong in silence. */
    DCHECK(cb_cap >= EVENT_FIRE_CB_SLOTS,
           "a fire request was handed a buffer narrower than §2.9's three-argument dispatch — declare it "
           "EventFireCb rather than counting the slots");

    if (*phase == 0) {
        JSValue fn = dispatch_fn_new(ctx);
        DCHECK(JS_IsObject(ev), "a synchronous fire was handed no event — §2.9 dispatches one that exists");
        argv[0] = target;
        argv[1] = ev;
        argv[2] = target_override;   /* §2.9 step 2's targetOverride — see event_target_fire */
        /* step_call_run DUPS the callee into the request buffer, which is what holds it across the suspension —
           so this realm's dispatcher is released here and the parked call still owns one. */
        r = step_call_run(ctx, phase, cb, cb_cap, fn, JS_UNDEFINED, 3, argv, in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        DCHECK(r == JS_STEP_CALL, "the dispatch request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 3, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "a synchronous fire resumed into something other than its answer");
    if (pnot_canceled) *pnot_canceled = JS_ToBool(ctx, out);
    JS_FreeValue(ctx, out);
    return 0;
}

static const JSTrampStepDef js_dispatch_pair_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_PAIR, .catches_abrupt = 1, .visit = js_dispatch_visit,
    .algorithm = "DOM §2.9 dispatch", .steps = DISPATCH_STEPS
};

/* §2.7's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — built at the end of the file because the dispatch machine
   it installs is declared just above. It is the FIRST entry in core/realm.h's list, so every realm — the
   agent's own included — has it before any interface that inherits EventTarget builds its prototype. */
static void event_target_install(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for EventTarget.prototype before event_target_init declared the interface");
    prev = JS_GetClassProto(ctx, g_et_class);
    DCHECK(JS_IsNull(prev),
           "event_target_install ran twice in one realm — §3.7 gives a realm ONE EventTarget.prototype, and a "
           "second would leave the objects already chained to the first answering out of a discarded one");
    JS_FreeValue(ctx, prev);
    /* THE DISPATCH DECLARATIONS are the RUNTIME's — a step def is registered against the runtime and there is
       one §2.9 machine — so they are declared once and every realm's members carry the same ids. */
    if (g_dispatch_stepid < 0) {
        g_dispatch_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_def);
        g_dispatch_pair_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_pair_def);
    }
    /* §2.7's `AbortSignal signal` CARRIES ITS OWN CLASS, and it is read HERE for the reason ADD_OPTS states:
       core/platform.c declares `event_target` far above `abort`, so at the declaration this id is still zero,
       and the two rows cannot swap because AbortSignal.prototype chains to the prototype built below. A class
       id is agent-scoped rather than per realm, so every realm's install reads and writes the same value —
       which is why this is not the module static §per-realm-fact forbids, and why abort_signal_class's own
       assert is what fires if the row order ever moves the other component after this one. It precedes the
       member installs deliberately: the conversion cannot run before its member exists on a prototype. */
    DCHECK(!strcmp(ADD_OPTS[ADD_OPTS_SIGNAL].name, "signal") &&
           ADD_OPTS[ADD_OPTS_SIGNAL].type == IDL_INTERFACE,
           "AddEventListenerOptions' member list moved under the index that carries its interface class — the "
           "list is in Web IDL §3.2.17 Dictionary types' read order, so inserting a member renumbers it and the "
           "brand would then be attached to a member whose type never asks for one. Asked BEFORE the write, "
           "because after it the wrong member is already branded");
    ADD_OPTS[ADD_OPTS_SIGNAL].iface = abort_signal_class();

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "EventTarget.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "EventTarget");
    idl_install_method(ctx, proto, "addEventListener", g_add_stepid);
    idl_install_method(ctx, proto, "removeEventListener", g_remove_stepid);
    idl_install_step_method(ctx, proto, "dispatchEvent", 1, g_dispatch_stepid);
    JS_SetClassProto(ctx, g_et_class, proto);   /* the realm owns it from here */
}
