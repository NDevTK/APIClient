/* EventTarget — DOM §2.7. See event_target.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#include <stdbool.h>
#include <stddef.h>
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
/* IT TAKES THE RUNTIME, which is what an AGENT is — see core/platform.h's release column. Nothing it gives back
   is a realm's: the per-realm prototype is held in that realm's class-proto slot and goes with the context, and
   what is left is two Symbols, a marker object, a class id and five step ids, all of them registrations in the
   runtime. It was a line in each of three hosts' hand-written teardowns for no reason but this signature. */
void event_target_free(JSRuntime *rt);

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
/* ONE CLAIMANT, AND NULL GIVES IT BACK. The slot is this component's state and the walk is another component's
   code, so the claimant releases it at ITS release — which core/platform.c's reverse-declaration order runs
   first, and event_target_free asserts. */
void event_target_set_tree(const EventTargetTree *tree);

/* THE `is_window` PREDICATE ABOVE, ASKED OF ONE TARGET — the registered answer, reachable by the algorithms
   that need it rather than only by the walk that happens to hold the struct. HTML §8.1.8.1's event handler
   processing algorithm step 4 is the second caller: its `special error event handling` is true only when the
   event's currentTarget "implements the WindowOrWorkerGlobalScope mixin", and the mixin's implementers are
   Window and WorkerGlobalScope — a worker global being a different agent with its own dispatch, which never
   reaches this walk. That is why the five-argument invocation is `window.onerror`'s and not `img.onerror`'s.
   False when no component has claimed the tree, which is the same answer §2.9's walk takes for a host with no
   tree at all: a target that is not in anyone's tree is not a global either. */
bool event_target_is_window(JSContext *ctx, JSValueConst target);

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
/* WEB IDL §3.2.15's `EventTarget?`, over the ONE value DOM §2.2 gives every Event — the associated
   relatedTarget, which §2.9 step 4 retargets and which MouseEventInit and FocusEventInit each declare a member
   over. It is stated HERE, once, because "does this value implement EventTarget" is this component's question
   and neither event interface's: written out in one of them, the second copy is a brand test a body wrote by
   hand, which is exactly what a declared type exists to replace.
   THERE IS NO SINGLE CLASS TO BRAND AGAINST — every Node, every Window, every MessagePort, every AbortSignal
   and every `new EventTarget()` implements the interface — so the question is asked of the object's PROTOTYPE
   CHAIN, which is where an interface's members actually live: a platform object implements EventTarget exactly
   when this realm's EventTarget.prototype is on its chain.
   `what` NAMES THE MEMBER being converted ("a FocusEvent's `relatedTarget`") and is the subject of the
   TypeError, because the one thing a page needs from it is which value it handed over was wrong.
   Answers JS_NULL / an owned dup, or JS_EXCEPTION with the TypeError live. */
JSValue event_target_nullable_of(JSContext *ctx, JSValueConst v, const char *what);
/* §2.7's INTERFACE PROTOTYPE OBJECT, where addEventListener, removeEventListener and dispatchEvent live.
   An interface that INHERITS EventTarget — Node, AbortSignal, MessagePort, BroadcastChannel, Window — chains
   its own prototype to this one; it does not install the three members again. That is not a saving, it is the
   spec: `EventTarget.prototype` is where they are declared, `Node.prototype` is not, and the corpus checks
   both. PER REALM — §3.7 gives each its own, and here that decides ANSWERS and not just identities, because a
   C member runs in the realm that defined it (see event_target.c). OWNED: the caller frees. */
JSValue event_target_proto(JSContext *ctx);
/* `interface X : EventTarget` — X's INTERFACE PROTOTYPE OBJECT, CREATED over §2.7's, in `ctx`'s realm. OWNED:
   the caller frees.
   IT CREATES RATHER THAN RE-PARENTS, and that is the whole point of it. Web IDL §3.7.3 Interface prototype
   object establishes the link AT CONSTRUCTION — "set proto to the interface prototype object in realm of that
   inherited interface", then "Set interfaceProtoObj to OrdinaryObjectCreate(proto)" — so there is no instant at
   which the object exists outside its chain. This was a JS_SetPrototype applied AFTER the object was built and
   tagged, which is observably the same only because nothing happened to read in between; the fourteen
   components that spelled it that way each had a window in which `X.prototype` answered Object.prototype, and
   idl_args.c's §3.7.3 proto-step assertion — which reads the parent's class string at the tag — is what made
   that window matter. */
JSValue event_target_derived_proto(JSContext *ctx);
/* §2.7's interface object. CONSTRUCTIBLE — `new EventTarget()` is a plain event target, which is how a page
   gives an ordinary object a listener list. */
void event_target_install_interface(JSContext *ctx, JSValueConst global);

/* HTML §8.1.8.1 Event handlers' EVENT HANDLER IDL ATTRIBUTES — `onclick`, `onload`, `onabort`. Which set a
   target carries is which MIXIN its IDL includes (§8.1.8.2.1 IDL definitions), so the caller names the mixin
   rather than the members.
   THE SECTION NUMBER WAS §8.1.7.2 HERE AND ONE LINE BELOW, and §8.1.7.2 is `Queuing tasks` — a real section
   about the event loop's task queues, which is what makes a wrong number worse than none: it reads as
   authoritative and sends the next reader to text that says nothing about event handlers. */
/* EH_XHR is XHR §3.3's set on XMLHttpRequestEventTarget — the seven a page uses to watch a transfer; it is a
   MIXIN's set, so XMLHttpRequestUpload gets exactly the same members by inheritance. EH_XHR_READYSTATE is the
   ONE §3.3 puts "solely" on XMLHttpRequest, which is why it cannot ride the same bit. */
/* EH_SHADOW_ROOT is DOM §4.8's `onslotchange`, which that interface declares ON ITSELF. It is also one of
   GlobalEventHandlers' names, so the one entry carries both bits — the mask is which MIXIN a target includes,
   and a name declared by two mixins is installed by both. */
/* EH_VISUAL_VIEWPORT is CSSOM VIEW §12's three, which that interface declares ON ITSELF. All three are also
   GlobalEventHandlers names, so each entry carries both bits — the mask is which MIXIN a target includes, and a
   name declared by two mixins is installed by both. */
/* EH_PERMISSION_STATUS is Permissions §6.3's `onchange`, which that interface declares ON ITSELF — a third
   owner of the same name, which is exactly what a bitmask is for. */
/* EH_NAVIGATION is HTML §7.2.6.2's set on Navigation and EH_NAVIGATION_HISTORY_ENTRY is §7.2.6.5's one on
   NavigationHistoryEntry, each declared ON its own interface. Neither name belongs to any other mixin, so
   `oncurrententrychange` and `ondispose` arrive in the one X-list through these two bits — and only the
   handlers whose events something actually fires are listed there, because an event handler IDL attribute for
   an event no algorithm dispatches is the shape-only member the IDL audit exists to expose. */
/* EH_IDB_REQUEST is Indexed Database §4.1's two on IDBRequest and EH_IDB_TRANSACTION is §4.10's three on
   IDBTransaction, each declared ON its own interface. `onerror` belongs to both AND to GlobalEventHandlers
   and XHR, and `onabort` to three owners already — which is exactly what a bitmask is for. */
/* EH_IDB_OPEN_REQUEST is §4.1's two on IDBOpenDBRequest — the "extended interface to allow listening to the
   blocked and upgradeneeded events", which is a SEPARATE bit from EH_IDB_REQUEST because the two names belong
   to the derived interface alone: an ordinary `store.get()` request must not answer `onupgradeneeded`.
   EH_IDB_DATABASE is §4.4's `onversionchange`, which is the one of that interface's four whose event an
   algorithm here FIRES — see the X-list for why the other three are not declared yet. */
/* EH_FILE_READER is File API §6.2.1 Event Handler Content Attributes' six on FileReader, which that section
   declares ON that interface ("the event handler content attributes … that user agents must support on
   FileReader as DOM attributes"). All six names already belong to GlobalEventHandlers and/or XHR §3.3's mixin,
   so each entry carries one more bit — which is exactly what a bitmask is for, and which is why a FileReader
   does NOT get them by including some other mixin: it includes none. */
/* EH_WINDOW_REFLECTING IS NOT A MIXIN AND IS THE ONE BIT NOBODY INSTALLS BY. It is HTML §8.1.8.2 Event handlers
   on elements, Document objects, and Window objects' WINDOW-REFLECTING BODY ELEMENT EVENT HANDLER SET — "the
   set of the names of the event handlers listed in the first column of this table", the table of the six that
   every HTML element OTHER THAN body and frameset supports as its own and that a `body`/`frameset` instead
   exposes on behalf of its Window. All six are GlobalEventHandlers names too, so they are installed by
   EH_GLOBAL exactly as before and this bit adds no member anywhere; what it marks is MEMBERSHIP, because
   §8.1.8.1's determine the target of an event handler step 2 tests the name against this set and against
   WindowEventHandlers' members (EH_WINDOW) and against nothing else. §4.3.1 The body element says those six,
   exposed on the body, "replace the generic event handlers with the same names normally supported by HTML
   elements" — REPLACE, not shadow: one accessor per name, whose target the determination decides. */
enum { EH_GLOBAL = 1, EH_WINDOW = 2, EH_DOCUMENT = 4, EH_SIGNAL = 8, EH_PORT = 16,
       EH_MEDIA_QUERY_LIST = 32, EH_XHR = 64, EH_XHR_READYSTATE = 128, EH_SHADOW_ROOT = 256,
       EH_VISUAL_VIEWPORT = 512, EH_PERMISSION_STATUS = 1024, EH_NAVIGATION = 2048,
       EH_NAVIGATION_HISTORY_ENTRY = 4096, EH_IDB_REQUEST = 8192, EH_IDB_TRANSACTION = 16384,
       EH_IDB_OPEN_REQUEST = 32768, EH_IDB_DATABASE = 65536, EH_FILE_READER = 131072,
       EH_WINDOW_REFLECTING = 262144,
       /* HTML §9.4.4 Message ports' OWN `onclose`, which is not the MessageEventTarget mixin's set:
          EH_PORT is `onmessage`/`onmessageerror`, which §9.5 Broadcasting to other browsing contexts
          includes on BroadcastChannel too, and no BroadcastChannel declares an `onclose`. A membership
          bit of its own is what keeps one name off a prototype whose IDL does not declare it. */
       EH_MESSAGE_PORT = 524288 };
/* HTML §6.5 Activation behavior of elements' click() — "Fire a synthetic pointer event named click at this
   element, with the not trusted flag set." — which IS DOM §2.9 dispatch, so it is the same machine under a
   second entry rather than a second implementation of it. */
void event_target_install_click(JSContext *ctx, JSValueConst target);
void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask);
/* IS THIS THE NAME OF AN EVENT HANDLER CONTENT ATTRIBUTE? HTML §8.1.8.1 defines that set as the names of the
   event handler IDL attributes above, so it is answered from the one list rather than from a second copy.
   Trusted Types §3.8 step 2 is the caller: an event handler content attribute maps to TrustedScript. */
bool event_target_is_handler_attribute(const char *name);
/* THE SAME SET, ENUMERATED. HTML §8.6.2's remove-unsafe step 4 appends every event handler content attribute
   to a configuration's removeAttributes list, which is a deny-list it must BUILD — a caller that can only ask
   "is this one" can filter an allow-list it already holds but cannot produce that. Both come off the one
   X-list, so a handler added to §8.1.8.1's set is added to both at once. The names are static. */
int         event_target_handler_attribute_count(void);
const char *event_target_handler_attribute_at(int i);

/* ---- THE CONTENT-ATTRIBUTE HALF OF §8.1.8.1, whose ALGORITHM lives in core/html/event_handler_attribute.c ---
 *
 * "Event handlers are exposed in TWO WAYS. The first way, common to all event handlers, is as an event handler
 * IDL attribute. The second way is as an event handler CONTENT attribute." The two ways write ONE handler — the
 * same entry of the same event handler map, whose §8.1.8.1 `value` is "either null, a callback object, or an
 * INTERNAL RAW UNCOMPILED HANDLER" — so a page that sets `<div onclick="a()">` and then `div.onclick = f` has
 * replaced one handler and not shadowed it, and the marker listener keeps the position the first write gave it.
 *
 * THE ALGORITHM IS NOT HERE AND THE PRIMITIVES ARE, and that split is the one this file already makes for
 * §2.9's propagation path. §8.1.8.1's attribute change steps need an ELEMENT (step 1's name test is per
 * element), a POLICY CONTAINER (step 5.1's "Should element's inline behavior be blocked by Content Security
 * Policy?") and a document — none of which this component may name: a `#include` of the DOM here made every
 * host that installs events link lexbor with it. So the steps live beside the element, in the HTML layer, where
 * their numbers are all visible in one function, and what crosses is the three algorithms §8.1.8.1 names in
 * their own right — determine the target, deactivate, and the map write that "activate an event handler"
 * follows. A hook answering "is this element a body" would have put HTML's algorithm here with nothing naming
 * its steps, which is the shape event_target_set_handler_target_terms below was written to avoid. */
/* WHICH ROW OF §8.1.8.2's TABLES `name` IS, or -1. ASCII case-insensitive, because `setAttributeNS` performs
   none of DOM §4.9 step 2's lowercasing and `onClick` in an XML document is not one of these names.
   (name, name_len) AND NEVER A NUL-TERMINATED NAME ALONE, because an attribute's local name is a (pointer,
   length) out of lexbor's own storage — a caller with one would otherwise have to copy it into a buffer whose
   size is a second, silent statement of how long the longest handler name is. */
int event_target_handler_attribute_index(const char *name, size_t name_len);
/* …AND WHETHER THAT NAME IS A CONTENT ATTRIBUTE ON AN ELEMENT, which is step 1's "localName is not the name of
   an event handler content attribute ON ELEMENT" and is NOT the same question as being one of the names.
   §8.1.8.2's FIRST table is "the event handlers … that must be supported by all HTML elements, as both event
   handler content attributes and event handler IDL attributes"; its THIRD is the eighteen WindowEventHandlers
   names, exposed as content attributes on body and frameset elements ALONE. So `<div onunload="x">` is an
   ordinary attribute in every browser and `<body onunload="x">` is a handler, and a test that asked only
   whether the name is in the list would have made the first one a handler on an object nothing dispatches
   `unload` at. §8.1.8.2's SECOND table needs no term of its own: all six of its names are GlobalEventHandlers
   members, so the first table already contains them and what that table decides is only their TARGET. */
bool event_target_handler_attribute_on_element(int index, bool body_or_frameset);
/* §8.1.8.1's DETERMINE THE TARGET OF AN EVENT HANDLER, by row rather than by name — the same four steps the IDL
   accessors run, so `<body onload=…>` and `document.body.onload = …` cannot land on different objects. OWNED,
   and JS_NULL is step 3's own third outcome rather than an error. */
JSValue event_target_determine_handler_target(JSContext *ctx, JSValueConst exposed, int index);
/* §8.1.8.1's DEACTIVATE AN EVENT HANDLER — set the handler's value to null and remove its listener. It is the
   attribute change steps' step 4 (a REMOVED attribute) and the IDL setter's step 3 (`el.onclick = null`), one
   algorithm reached two ways. `target` is the DETERMINED target, never the object the handler is exposed on. */
void event_target_deactivate_handler(JSContext *ctx, JSValueConst target, int index);
/* §8.1.8.1's attribute change steps 5.2, 5.3, 5.5 and 5.6 — "let handlerMap be eventTarget's event handler
   map", "let eventHandler be handlerMap[localName]", "set eventHandler's value to the INTERNAL RAW UNCOMPILED
   HANDLER value/location", "activate an event handler". Steps 5.1 (CSP) and 5.4 (the location) are the
   caller's, because both are facts about the element and the script that wrote the attribute rather than about
   the handler map. `target` is the DETERMINED target. `body` is BORROWED and is not NUL-terminated — an
   attribute value is a (pointer, length) out of the DOM, and a handler body may legitimately contain U+0000.
   NOTHING COMPILES IT YET: §8.1.8.1's "get the current value of the event handler" step 3 is what turns this
   value into a function, and reading one before that exists CRASHES by name rather than answering. */
void event_target_set_uncompiled_handler(JSContext *ctx, JSValueConst target, int index,
                                         const char *body, size_t body_n);

/* A handler attribute whose SETTER has a side effect. HTML has one: §9.4.2's `onmessage` on a MessagePort also
   starts the port, which is why assigning it is enough and addEventListener alone is not. The hook runs AFTER
   the handler is registered — start() delivers what is already queued, and delivering it first would fire at a
   target with no listener yet — and it is given the target and the attribute name so the registering component
   decides with its own brand test rather than this file knowing what a MessagePort is. */
void event_target_set_handler_hook(void (*after_set)(JSContext *ctx, JSValueConst target, const char *name));

/* HTML §8.1.8.1 Event handlers' DETERMINE THE TARGET OF AN EVENT HANDLER — the three DEFINED TERMS its four
 * steps are composed of, registered the way §2.9's propagation path is and for the same reason: the ALGORITHM
 * stays here where its step numbers are visible, and the questions it asks of the tree are answered by the
 * component that can answer them. A hook that answered "does this delegate" instead would put HTML's algorithm
 * in the HTML layer with nothing naming the steps.
 *
 * WHAT IT IS FOR. "Most of the time, the object that exposes an event handler is the same as the object on
 * which the corresponding event listener is added. However, the body and frameset elements expose several
 * event handlers that ACT UPON THE ELEMENT'S Window OBJECT, if one exists." Without the determination,
 * `document.body.onload = f` registers a `load` listener ON THE BODY — and §13.2.7 "The end" step 9.5 fires
 * `load` AT THE WINDOW with the legacy target override flag, a dispatch whose propagation path the body is not
 * on at all. So the handler is never invoked and the page's entry point never runs; `<body onload="init()">`
 * and `document.body.onload = init` are two of the commonest ways a bundle starts.
 *
 *   `is_body_or_frameset` is step 1's test — "eventTarget is not a body element or a frameset element". It is
 *     the ELEMENT TYPE and not the document's `body`: §8.1.8.1's own note says a body element created with
 *     `createElement()` in an active document and never connected still has that document's Window as its
 *     target, so a `document.body ===` test would answer no for exactly that case.
 *   `node_document_is_active` is step 3's — HTML §7.3.1 Navigables' ACTIVE DOCUMENT, asked of eventTarget's
 *     node document. False makes the determination answer NULL, which is a THIRD outcome and not a fallback to
 *     the element: the getter then returns null and the setter does nothing at all, so a handler assigned to a
 *     body in a `DOMParser` document is silently dropped exactly as a browser drops it.
 *   `node_document_global` is step 4's answer — "eventTarget's node document's relevant global object".
 *     BORROWED; the realm owns its global.
 *
 * ALL THREE OR NONE, and a host that registers none has no body elements to ask about, which is the same
 * answer event_target_is_window takes for a host with no tree. */
typedef struct EventHandlerTargetTerms {
    bool         (*is_body_or_frameset)(JSContext *ctx, JSValueConst target);
    bool         (*node_document_is_active)(JSContext *ctx, JSValueConst target);
    JSValueConst (*node_document_global)(JSContext *ctx, JSValueConst target);
} EventHandlerTargetTerms;
/* ONE CLAIMANT, AND NULL GIVES IT BACK — see event_target_set_tree for why the two are one call. */
void event_target_set_handler_target_terms(const EventHandlerTargetTerms *terms);

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

/* DOM §2.7 Interface EventTarget's "ERASE ALL EVENT LISTENERS AND HANDLERS, given an EventTarget eventTarget" —
   both maps, in one call, because §8.1.8.1's handler lives in both of them. HTML §8.4.1 "Opening the input
   stream" steps 9 and 10 are the caller: every shadow-including inclusive descendant of the document, and then
   the relevant global object. See the body for why it replaces the maps rather than deleting the slots. */
void event_target_erase_all(JSContext *ctx, JSValueConst target);

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
   ceiling `window.open` had while it was a plain C body.
   ONE CLAIMANT, AND A NULL PAIR GIVES IT BACK — see event_target_set_tree above for why. */
void event_target_set_activation(bool (*has)(JSContext *ctx, JSValueConst el),
                                 int (*run)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                                            uint8_t *phase, uint32_t *req));
/* The ENGINE firing its own event at `target`, ON A TASK SOURCE. One §2.9 dispatch, and the caller's standard
   is what selects this reach rather than the request one below: it is for a caller whose spec says "QUEUE a
   task … to fire an event named X" and for no other. A fire the standard states as a bare synchronous step —
   HTML §3.1.5 "Reporting document loading status"' `readystatechange`, §6.2 "Page visibility"'s
   `visibilitychange`, the fires INSIDE the queued task of §9.3.3 "Posting messages", §9.4.4 "Message ports"
   and §9.5 "Broadcasting to other browsing contexts" — belongs to event_target_fire_run, whose caller is
   already a step machine. Queuing one of those here does not make it "asynchronous rather than synchronous",
   it puts the page's listener AFTER work the standard puts it before.
   The propagation path is derived from the target's ancestors — there is no
   `bubble_to` to pass, because the window is the document's parent and the spec already says so.
   It takes the EVENT, not a type and two flags: §2.9 dispatches an event, and a caller with a DERIVED one
   (PromiseRejectionEvent) has no way to hand it over if this mints its own. `ev` is CONSUMED. */
/* `target_override` is §2.9 step 2's targetOverride — JS_UNDEFINED for an ordinary dispatch. HTML gives one
   for `pagehide`, `pageshow`, `unload` and `beforeunload`, which are fired AT the Window with the DOCUMENT as
   their target (what the spec spells as the legacy target override flag). It is the VALUE rather than a
   boolean because the flag's whole content is "the target's associated Document" and the caller holds it;
   asked as a boolean this component would have to resolve a Window whose realm it may not own. */
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev, JSValueConst target_override);
/* THE FIRE REQUEST BUFFER, AS A TYPE. §2.9's dispatcher takes THREE arguments (target, event, targetOverride)
   and step_call_run's operand shape is [this, func, args…], so the buffer is 2 + 3 slots wide.
   IT IS A TYPE BECAUSE A WIDTH EVERY CALLER RESTATES IS A WIDTH EVERY CALLER IS FREE TO BE BEHIND ON.
   `target_override` was added to the dispatch as a third argument and its own doc-comment went on saying FOUR
   slots, so fifteen callers declared `JSValue cb[4]` for a call needing five and the FIRST fire of the run —
   the document's own `DOMContentLoaded` — dupped the override one slot past the end of the array, over
   whatever struct field happened to follow it. It aborted in step_call_run's capacity DCHECK, which is the
   lucky version; the unlucky one is a buffer whose neighbour is padding and which corrupts nothing until a
   fork copies it. A caller that names this type cannot be an argument behind the algorithm, because there is
   no number at the call site to be behind with. */
#define EVENT_FIRE_CB_SLOTS (2 + 3)
typedef JSValue EventFireCb[EVENT_FIRE_CB_SLOTS];

/* THE SAME FIRE for a caller that can park — §2.9 is synchronous, and §3.2's `abort` is specified that way. One
   dispatch, two reaches: this is the REQUEST form, event_target_fire is the queued one. `phase` and `cb` belong
   to the calling machine; `cb` is an `EventFireCb` passed through STEP_CB so its capacity travels with it, as a
   forwarded buffer can no longer be measured. `ev` is the caller's too: the CALLER owns it and holds it across
   the suspension, because §2.9 dispatches an event that exists rather than one the dispatch invents. Returns
   JS_STEP_CALL (return it) or 0 when it has answered. */
int  event_target_fire_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst target,
                           JSValueConst ev, JSValueConst target_override, JSValue in,
                           bool *pnot_canceled, JSValue **out_cb, int *out_argc);

#endif
