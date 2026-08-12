/* EventTarget — DOM §2.7. See event_target.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

void event_target_init(JSContext *ctx);                          /* the private listener key (agent init) */
/* §2.7's PROTOTYPE FOR ONE REALM. Run it where a realm's other intrinsics are added — at the realm's creation,
   beside JS_AddIntrinsicDOMException — and exactly once per realm. The agent's first realm gets it from
   event_target_init, because every agent-scoped prototype in this engine chains to that realm's and so it has
   to exist before them; a child navigable's realm gets it from its host's realm builder. */
/* Release that key. A component that mints a RUNTIME-LIFETIME value owns it, and this one did not free its
   Symbol — so every instance leaked it. It was invisible while only the ABI entry installed listeners, because
   nothing there runs the leak check; the moment the fixture harness installed the same components it ships
   with, JS_FreeRuntime's gc_obj_list assert named it. */
void event_target_free(JSContext *ctx);

/* WHAT THE EVENTS LAYER HAS TO ASK THE TREE. Two questions, and both are the DOM's rather than this file's, so
   the DOM registers them rather than this file naming node.c — which is what let the streams gate build with a
   real AbortSignal and no lexbor in it.
   `get_parent` IS §2.9's "get the parent": a node's is its parent, a DOCUMENT's is its window unless the event
   is `load` or it has no browsing context, and anything that is not in a tree answers NULL — which is §2.7's own
   default and gives a path of one. It replaced an "ancestors" list, and the difference is not a shape: the list
   could not say that a document's parent is a window while a DETACHED node's is nothing, so the walk appended
   the realm's window above every detached node and above a document dispatching `load`.
   `default_passive_target` is §2.7's default passive value, minus the type test this file makes: is this target
   the window, the node document, its document element, or its body. OWNED BY THE CALLER and must outlive the
   runtime — a static, as node.c's is. */
/* §4.8's MODE, asked of any EventTarget rather than of a shadow root, because §2.9 asks it of path entries it
   has not established are shadow roots at all. */
enum { EVENT_TREE_NOT_SHADOW_ROOT = -1, EVENT_TREE_SHADOW_OPEN = 0, EVENT_TREE_SHADOW_CLOSED = 1 };

/* AND THE SEVEN SHADOW FACTS §2.9's WALK AND §4.8's RETARGETING NEED, each one a DEFINED TERM of the standard
   rather than a decision.
   That split is the point: which COMBINATION of them retargets the event, hides a path entry or picks the
   activation target is §2.9's algorithm and stays in the dispatch machine; what a "shadow root", a "slot", an
   "assigned slottable" or a "shadow-including inclusive ancestor" IS belongs to the DOM, which is the only
   component that can answer it and the only one that may link lexbor to do so. A hook that answered a BRANCH
   instead ("does the event cross a boundary here") would put the standard's algorithm in node.c, where the step
   numbers it implements are invisible.
     `root` is §4.4's root — the topmost inclusive ancestor, which for a node inside a shadow tree is the SHADOW
       ROOT and not the document. OWNED; JS_NULL for anything that is not a node, which is also how the walk
       tells a Window from a node without a second question.
     `shadow_root_mode` answers EVENT_TREE_NOT_SHADOW_ROOT for everything that is not a shadow root.
     `is_window` is §2.9 step 6.9.5's first disjunct.
     `is_slot` is what step 6.9.1's assert is about.
     `is_assigned_slottable` is §4.2.2.2's "a slottable is assigned", which is also what makes a node's get the
       parent answer with its slot.
     `is_shadow_including_inclusive_ancestor` is §4.2's relation, asked as "is `a` one of `b`'s" — the relation
       that climbs from a shadow root to its HOST, which is why it cannot be a parent-chain walk in this file.
     `shadow_host` is §4.8's HOST of a shadow root, and it is the one fact none of the others can stand in for:
       §4.8's retargeting step 2 is "set A to A's root's host", the single CLIMB in the standard that leaves a
       tree for the thing containing it. `root` goes up INSIDE a tree and stops at its shadow root, and the
       ancestor relation only TESTS — so without this, retarget(A, B) can decide that A must be hidden and has
       nothing to answer with. OWNED; JS_NULL for anything that is not a shadow root. */
typedef struct EventTargetTree {
    JSValue (*get_parent)(JSContext *ctx, JSValueConst target, JSValueConst ev);
    bool    (*default_passive_target)(JSContext *ctx, JSValueConst target);
    JSValue (*root)(JSContext *ctx, JSValueConst target);
    int     (*shadow_root_mode)(JSContext *ctx, JSValueConst target);
    bool    (*is_window)(JSContext *ctx, JSValueConst target);
    bool    (*is_slot)(JSContext *ctx, JSValueConst target);
    bool    (*is_assigned_slottable)(JSContext *ctx, JSValueConst target);
    bool    (*is_shadow_including_inclusive_ancestor)(JSContext *ctx, JSValueConst a, JSValueConst b);
    JSValue (*shadow_host)(JSContext *ctx, JSValueConst shadow_root);
} EventTargetTree;
void event_target_set_tree(const EventTargetTree *tree);

/* DOM §4.8's RETARGETING ALGORITHM — "to retarget an object A against an object B", the operation that decides
   what an object inside a shadow tree is CALLED when it is reported to something outside it. It is not an event
   algorithm and does not belong to any one caller: §2.9 runs it four times (the event's relatedTarget and each
   of its touch targets, against the target and then against every ancestor) and Fullscreen runs it too, and the
   standard states it ONCE. It lives in this file because the tree is registered here and the algorithm is three
   tree questions and one climb — nothing else of it is this component's.
   `a` is a potential event target and so is the ANSWER, which is OWNED. Never inline it at a call site: every
   site would then have to know that "A is not a node" is answered by A having no root and that the climb is a
   LOOP, and the first site to get either wrong reports a node out of a closed tree to a listener that must not
   see it. */
JSValue event_target_retarget(JSContext *ctx, JSValueConst a, JSValueConst b);
/* §2.7's INTERFACE PROTOTYPE OBJECT, where addEventListener, removeEventListener and dispatchEvent live.
   An interface that INHERITS EventTarget — Node, AbortSignal, MessagePort, BroadcastChannel, Window — chains
   its own prototype to this one; it does not install the three members again. That is not a saving, it is the
   spec: `EventTarget.prototype` is where they are declared, `Node.prototype` is not, and the corpus checks
   both. PER REALM — §3.7 gives each its own, and here that decides ANSWERS and not just identities, because a
   C member runs in the realm that defined it (see event_target.c). OWNED: the caller frees. */
JSValue event_target_proto(JSContext *ctx);
/* `interface X : EventTarget` — chain X's prototype to §2.7's, in `ctx`'s realm. Four interfaces declare it and
   each spelled it as a borrowed read fed straight to JS_SetPrototype; now that the read is per-realm and owned,
   the pair is written once here rather than four times, all four of them free-or-leak. */
void event_target_chain(JSContext *ctx, JSValueConst proto);
/* §2.7's interface object. CONSTRUCTIBLE — `new EventTarget()` is a plain event target, which is how a page
   gives an ordinary object a listener list. */
void event_target_install_interface(JSContext *ctx, JSValueConst global);

/* HTML §8.1.7.2 EVENT HANDLER IDL ATTRIBUTES — `onclick`, `onload`, `onabort`. Which set a target carries is
   which MIXIN its IDL includes, so the caller names the mixin rather than the members. */
/* EH_XHR is XHR §3.3's set on XMLHttpRequestEventTarget — the seven a page uses to watch a transfer; it is a
   MIXIN's set, so XMLHttpRequestUpload gets exactly the same members by inheritance. EH_XHR_READYSTATE is the
   ONE §3.3 puts "solely" on XMLHttpRequest, which is why it cannot ride the same bit. */
/* EH_SHADOW_ROOT is DOM §4.8's `onslotchange`, which that interface declares ON ITSELF. It is also one of
   GlobalEventHandlers' names, so the one entry carries both bits — the mask is which MIXIN a target includes,
   and a name declared by two mixins is installed by both. */
enum { EH_GLOBAL = 1, EH_WINDOW = 2, EH_DOCUMENT = 4, EH_SIGNAL = 8, EH_PORT = 16,
       EH_MEDIA_QUERY_LIST = 32, EH_XHR = 64, EH_XHR_READYSTATE = 128, EH_SHADOW_ROOT = 256 };
/* HTML §3.2.2 click() — "fire a synthetic pointer event named click", which IS §2.9 dispatch, so it is the same
   machine under a second entry rather than a second implementation of it. */
void event_target_install_click(JSContext *ctx, JSValueConst target);
void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask);
/* IS THIS THE NAME OF AN EVENT HANDLER CONTENT ATTRIBUTE? HTML §8.1.7.2 defines that set as the names of the
   event handler IDL attributes above, so it is answered from the one list rather than from a second copy.
   Trusted Types §3.8 step 2 is the caller: an event handler content attribute maps to TrustedScript. */
bool event_target_is_handler_attribute(const char *name);
/* THE SAME SET, ENUMERATED. HTML §8.6.2's remove-unsafe step 4 appends every event handler content attribute
   to a configuration's removeAttributes list, which is a deny-list it must BUILD — a caller that can only ask
   "is this one" can filter an allow-list it already holds but cannot produce that. Both come off the one
   X-list, so a handler added to §8.1.7.2's set is added to both at once. The names are static. */
int         event_target_handler_attribute_count(void);
const char *event_target_handler_attribute_at(int i);
/* A handler attribute whose SETTER has a side effect. HTML has one: §9.4.2's `onmessage` on a MessagePort also
   starts the port, which is why assigning it is enough and addEventListener alone is not. The hook runs AFTER
   the handler is registered — start() delivers what is already queued, and delivering it first would fire at a
   target with no listener yet — and it is given the target and the attribute name so the registering component
   decides with its own brand test rather than this file knowing what a MessagePort is. */
void event_target_set_handler_hook(void (*after_set)(JSContext *ctx, JSValueConst target, const char *name));

/* §2.7's "add an event listener" AND "remove an event listener", reached from C with the type already a real
   string. The callers are members of OTHER standards that the spec DEFINES as those algorithms rather than as
   lists of their own: CSSOM VIEW §4.2's `MediaQueryList.addListener(callback)` IS `addEventListener("change",
   callback)`, which is why `mql.addListener(f)` and `mql.removeEventListener("change", f)` name one
   registration in every browser. A component keeping its own list would answer that pair with two.
   The CALLBACK is the page's value and is stored as-is; nothing here runs it, so neither of these can reach the
   page's code and neither needs to be a request.

   THE ADD TAKES THE WHOLE LISTENER, not a callback and three defaults. §2.7's listener is five fields, and a C
   caller that could only ever register (capture false, once false, passive default, no signal) could not
   express the one registration another standard actually asks for: the Observable standard's §3 `when()` names
   FOUR of the five explicitly — its `capture` and `passive` come from an `ObservableEventListenerOptions`
   dictionary, and its `signal` is the subscription controller's, which is the entire mechanism by which
   unsubscribing removes the listener. Narrowing the entry point to the fields the first caller happened to
   need is how a component ends up keeping a listener list of its own.
   `passive` is the TRISTATE the flatten-more-options algorithm makes it: -1 means the caller did not say, and
   §2.7's step 4 then fills it from the default passive value for the type. `signal` may be undefined. */
void event_target_add_listener(JSContext *ctx, JSValueConst target, const char *type, JSValueConst cb,
                               bool capture, bool once, int passive, JSValueConst signal);
void event_target_remove_listener(JSContext *ctx, JSValueConst target, const char *type, JSValueConst cb);

/* IS ANYTHING REGISTERED ON THIS TARGET AT ALL — §2.7's event listener list, asked as "is it non-empty" for
   ANY type. XHR §3.5.6 step 5 is the caller and the question is exactly that: "if one or more event listeners
   are registered on this's upload object", which is what sets the upload listener flag and, in a real browser,
   what makes the request CORS-preflighted. It is asked of this file rather than counted by the caller because
   an event handler attribute (`upload.onprogress = f`) registers a listener too, and a component keeping its
   own count would answer no for exactly the way most pages write it. */
bool event_target_has_any_listener(JSContext *ctx, JSValueConst target);

/* DOM §2.9's ACTIVATION BEHAVIOUR — what makes a click on an `<a href>` FOLLOW the link, on a `<form>`'s submit
   button submit, on a checkbox toggle it. It is not a listener and a page cannot register one: the dispatch
   picks an ACTIVATION TARGET while it builds the propagation path — the nearest entry, target first, that HAS
   one — and runs it AFTER the walk, only if nothing called preventDefault. That "only if" is the whole of what
   `preventDefault` means on a click, and with no activation behaviour at all it meant nothing: §2.9 ran its
   three legs and then dropped the event, so `<a href>` clicked navigated nowhere and `e.preventDefault()`
   suppressed something that was never going to happen.
   The two halves are declared by whoever owns the element, for the reason the tree walk is: this file does not
   know what an `<a>` is. `has` answers whether that element has one; `run` PERFORMS it, and it performs it as a
   STEP: §4.6.3's is a navigation, navigating fetches, and a fetch is a host-owed answer that suspends the
   asking flow. So `run` has a step's return contract — JS_STEP_YIELD to be re-entered, JS_STEP_DONE when it is
   finished — and its own two words of state on the dispatch machine, which is already a step machine and can
   therefore hold the suspension. A `void` hook could only ever reach a SYNCHRONOUS behaviour, which is the same
   ceiling `window.open` had while it was a plain C body. */
void event_target_set_activation(bool (*has)(JSContext *ctx, JSValueConst el),
                                 int (*run)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                                            uint8_t *phase, uint32_t *req));
/* The ENGINE firing its own event at `target`. One §2.9 dispatch, reached as a queued task because the callers
   are plain C the scheduler drives. The propagation path is derived from the target's ancestors — there is no
   `bubble_to` to pass, because the window is the document's parent and the spec already says so.
   It takes the EVENT, not a type and two flags: §2.9 dispatches an event, and a caller with a DERIVED one
   (PromiseRejectionEvent) has no way to hand it over if this mints its own. `ev` is CONSUMED. */
/* `target_override` is §2.9 step 2's targetOverride — JS_UNDEFINED for an ordinary dispatch. HTML gives one
   for `pagehide`, `pageshow`, `unload` and `beforeunload`, which are fired AT the Window with the DOCUMENT as
   their target (what the spec spells as the legacy target override flag). It is the VALUE rather than a
   boolean because the flag's whole content is "the target's associated Document" and the caller holds it;
   asked as a boolean this component would have to resolve a Window whose realm it may not own. */
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev, JSValueConst target_override);
/* THE SAME FIRE for a caller that can park — §2.9 is synchronous, and §3.2's `abort` is specified that way. One
   dispatch, two reaches: this is the REQUEST form, event_target_fire is the queued one. `phase` and `cb` belong
   to the calling machine and `cb` needs FOUR slots — pass it through STEP_CB so its capacity comes with it, as
   a forwarded buffer can no longer be measured. `ev` is the caller's too: the CALLER owns it and holds it across
   the suspension, because §2.9 dispatches an event that exists rather than one the dispatch invents. Returns
   JS_STEP_CALL (return it) or 0 when it has answered. */
int  event_target_fire_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst target,
                           JSValueConst ev, JSValueConst target_override, JSValue in,
                           bool *pnot_canceled, JSValue **out_cb, int *out_argc);

#endif
