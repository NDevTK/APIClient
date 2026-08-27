/* THE WINDOW INTERFACE — Blink core/frame, the browsing-context half of the global object.
 *
 * WHY THIS IS A COMPONENT AND NOT TWO LINES IN main.c. `window` and `self` were assigned there directly, which
 * covered the two names a bundle spells most often and nothing else. The rest of the browsing-context surface
 * — `parent`, `top`, `frames`, `opener`, `closed` — was simply missing, and a missing property on the global
 * is `undefined`, not a throw: 139 of 175 WPT dom/nodes documents died inside testharness.js's
 *
 *     var w = self; while (w != w.parent) { w = w.parent; ... }
 *
 * with "cannot read property 'parent' of undefined" on the SECOND iteration. A real browser ends that loop
 * immediately because a top-level window's `parent` IS the window.
 *
 * WHICH BROWSING CONTEXT IS THIS. HTML 7.2.2 answers window/self/frames with this Window's own WindowProxy,
 * and answers parent/top with the parent (or top) navigable's proxy, falling back to this one when there is
 * none. This engine runs ONE document per instance — SECURITY.md's one-WASM-instance-per-DOCUMENT — and holds
 * no channel to an embedder, so within the world this instance models the document IS its own top: parent and
 * top are the window. That is the concrete truth of the modelled context, not a shrug: making it concolic
 * would model an ignorance the engine does not have, and would fork the loop above without end. When the
 * cross-WASM chain that lets an embedded document reach its embedder exists, parent/top resolve through it and
 * this is where that lands.
 *

 * WHAT IT DOES NOT DO. This installed the PLATFORM GLOBALS as well — URL, URLSearchParams, FormData, Blob, the
 * four stream interfaces, TextEncoder/Decoder and their stream forms. None of them is a browsing-context
 * member and none belongs to this component; they were here only because main.c happened to be the one caller.
 * That cost a real gate: the WPT runner installs every one of those itself, so it could not call this without
 * double-installing, and it therefore had NO browsing-context members at all — `window` was not defined, and
 * every test in html/browsers/the-window-object failed on its first line. Two jobs in one function is what
 * made the second caller impossible; each install now sits with its own caller.
 *
 * `name` IS THE NAVIGABLE'S — §7.2.2.1's, the same attribute a WindowProxy answers — so it is not computed here
 * at all; window_proxy_name_value is. It is ATTACKER INPUT exactly when nobody stated it: the name survives
 * navigation, so whoever opened the document sets it, which is why CLAUDE.md lists it beside cookies and the
 * referrer — and it is a computed value when this engine's own `open(url, target)` named the navigable. Read
 * through a GETTER either way, for the same reason location.hash is: a candidate run substitutes a source at
 * MINT time, and a source minted once at install could never receive a breakout. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/window.h"
#include "core/frame/agent_cluster.h"
#include "core/frame/secure_context.h"
#include "core/frame/document_lifecycle.h"
#include "core/frame/window_proxy.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "core/frame/bar_prop.h"
#include "core/html/html_iframe.h"
#include "core/html/focus.h"
#include "core/dom/selection.h"
#include "core/events/event_target.h"
#include "core/dom/collections.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "solver/cow.h"

/* §7.2.2.1's `closed` IS THE NAVIGABLE'S, not the Window's — which is why it is not read from a byte here.
   `window.closed` and `iframe.contentWindow.closed` are the SAME fact about the SAME navigable, and a byte in
   this file made them two: closing through one left the other reporting open. The navigable's state lives on
   its WindowProxy (per-flow, captured into the running flow's delta like every other binding it holds), and
   §7.2.3 gives a navigable exactly one proxy — so reading it there is reading the one answer. */
static JSValue js_win_closed(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    return JS_NewBool(ctx, window_proxy_closed(ctx, nav));
}

/* HTML §8.1.7.1's WindowOrWorkerGlobalScope: `readonly attribute boolean isSecureContext`. "The
   isSecureContext getter steps are to return true if this's relevant settings object is a secure context, or
   false otherwise" — §8.1.3.5's algorithm, which secure_context.c owns, over THIS realm's environment. A C
   member runs in the realm that DEFINED it, so `ctx` is this document's and an `http` iframe of an `https`
   page answers out of its own environment rather than out of whichever realm was built first.
   IT IS COMPUTED, NEVER CONCOLIC. This engine's documents have real addresses, so a realm's secure-context
   answer is a fact the engine HAS — CLAUDE.md's rule is that the concolic value is for what is unknowable, and
   forking a boolean whose sibling world does not exist spends the frontier on a document that was never
   loaded. The other arm is reached by exploring a document at a different ADDRESS, which is a real navigation.
   IT IS AN ACCESSOR AND NOT A STORED BOOLEAN because §7.4.2.2's navigation replaces a Window's Document while the
   global survives, and a byte written at install would then be the previous document's answer. */
static JSValue js_win_is_secure_context(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    /* "THIS's relevant settings object", and `secure_context_is` answers for a REALM. The two agree for every
       receiver that is this realm's own navigable and for §3.7.6's missing-receiver fallback, which is every
       way this member is reached today; a receiver naming ANOTHER navigable is a question about that
       document's environment, and answering it out of this realm would be a boolean about the wrong document
       — indistinguishable, at the call site, from the right one. What it needs is the environment of the
       navigable's ACTIVE DOCUMENT, which is the same realm-of-a-navigable edge §7.2.2.2's `length` wants one
       member down. */
    DCHECK(JS_VALUE_GET_PTR(nav) == JS_VALUE_GET_PTR(document_window_proxy(ctx)),
           "§8.1.7.1's isSecureContext was read with ANOTHER navigable as its receiver — §8.1.3.5 answers for "
           "THIS's relevant settings object and secure_context_is answers for a realm, so build the edge from "
           "a navigable to its active document's realm and ask that one");
    return JS_NewBool(ctx, secure_context_is(ctx));
}

/* §7.2.2.1 `close()`. THE METHOD IS ONE ALGORITHM AND THIS IS ONE OF ITS TWO SPELLINGS — `window.close()` here
   and `w.close()` through the WindowProxy are the same six steps on the same navigable, and each carried a body
   of its own that was step 2's early return and then a single byte: is closing went true and nothing else
   happened. So `closed` reported a closed window over a document that was still running — its beforeunload and
   unload listeners never fired, its timers stayed scheduled, its subframes stayed live. Steps 4-6 are what was
   missing, and they live in document_lifecycle.c because what step 6 queues is §7.3's definitely close, which
   unloads the subtree and then destroys it. */
static JSValue js_win_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)argc; (void)argv; (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    document_lifecycle_window_close(ctx, nav);
    return JS_UNDEFINED;
}

/* §6.6.6's `Window.blur()`, whose METHOD STEPS ARE "TO DO NOTHING" — the standard's own words, and the note
   beside them says why: "historically, the focus() and blur() methods actually affected the system-level focus
   of the system widget that contained the navigable, but hostile sites widely abuse this behavior to the user's
   detriment". So this is the SPEC's no-effect and not this engine's; `Window.focus()` is NOT one of these and
   is no longer here — §6.6.6 gives it real steps (the navigable, the allow focus steps, then §6.6.4's focusing
   steps), which core/html/focus.c runs. `stop()` shares this body because this engine has no in-flight
   navigation to abort. */
static JSValue js_win_noeffect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

/* §7.2.2.2's `length`: the number of DOCUMENT-TREE CHILD NAVIGABLES. A GETTER over a real walk, never a stored
   number — a page appends a frame, removes it and reads this, and a count something forgot to adjust is wrong
   in exactly that case. It was absent entirely, which is `undefined` rather than 0 and is not the same answer
   for any page that tests it. */
static JSValue js_win_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    /* THE WALK IS A REALM'S AND THE MEMBER IS A NAVIGABLE'S, and for `length` those are not the same question
       the moment the receiver is another navigable: §7.2.2.2 counts THIS's associated Document's child
       navigables, and this realm's walk counts this document's. The WindowProxy spelling of the same member
       already answers it for any navigable (§7.2.3's WP_LENGTH), but it does so as a STEP MACHINE because the
       document may live in another instance, and a plain getter has no driver to run one on. So the receiver
       is resolved and the one case this body cannot answer CRASHES rather than counting the wrong
       document's frames. */
    DCHECK(JS_VALUE_GET_PTR(nav) == JS_VALUE_GET_PTR(document_window_proxy(ctx)),
           "§7.2.2.2's `length` was read with ANOTHER navigable as its receiver, and this body counts THIS "
           "realm's child navigables. The proxy spelling answers it for any navigable and suspends where the "
           "document is a peer's — declare this member a step machine over that same body so both spellings "
           "are one answer, which is what §7.2.2.1's `closed` already is");
    return JS_NewInt32(ctx, iframe_child_navigable_count(ctx));
}

/* HTML §7.2.2.4 "Accessing related windows"' `frameElement` getter steps, in the standard's own order: "Let
   current be this's node navigable. If current is null, then return null. Let container be current's container.
   If container is null, then return null. If container's node document's origin is not same origin-domain with
   the current settings object's origin, then return null. Return container."
   IT WAS A FIXED `JS_NULL` INSTALLED ON EVERY REALM, which answered the top-level case correctly and every
   child navigable WRONGLY, as a plain data property, with a comment calling null "the real answer for what this
   is". Then it was a DFAIL naming the edge to build. The edge is built — §7.3.1.3's container, recorded by
   create-a-new-child-navigable and confirmed against the element's own content navigable
   (window_proxy_container) — so the four steps are four lines.
   STEP 5 IS SAME ORIGIN-DOMAIN AND IT CAN FAIL INSIDE ONE INSTANCE, which is why it is asked rather than argued
   away. The comment this replaces claimed the question could not fail here, on the grounds that an instance is
   an origin-keyed agent cluster and a cross-origin child lives in a peer. Same ORIGIN is what that argument is
   about; §7.1.1's same origin-domain is a different algorithm, and two documents of one agent cluster differ
   under it the moment one of them runs §7.1.1.2's `document.domain` setter and the other does not — the
   standard's own fourth table row, and the same distinction §7.3.1's `content document` filter already draws
   one file over. The origin compared is the CONTAINER's node document's, which §7.3.1.3's `container document`
   defines as exactly that: "Return navigable's container's node document" — the active document of the
   navigable this one is nested in. */
static JSValue js_win_frame_element(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);
    JSValue container, parent;
    bool same_origin_domain;

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    /* Steps 1-2. §7.2.2's "the navigable whose active document is document" has no answer once the navigable
       has been detached from the tree or its Document destroyed — one question, asked where `top` and `parent`
       ask it (window_proxy.h). IT WAS `window_proxy_destroyed` HERE, which is only half of the fact and the
       half that arrives LATE: §7.5.10's destroy is a queued task, while §7.3.1.6 step 3's sever happens inside
       the removing steps, so this answered a container ELEMENT for a frame whose own subtree had already been
       detached — steps 3-4 saved it for the removed frame itself, whose element's slot is cleared on that same
       line, and could not save it for anything nested inside one. */
    if (window_proxy_navigable_null(ctx, nav)) return JS_NULL;
    /* Steps 3-4: §7.3.1.3's container, null for a navigable nested through nothing. */
    container = window_proxy_container(ctx, nav);
    if (JS_IsNull(container)) return JS_NULL;
    /* Step 5. The container's node document is the parent navigable's active document (§7.3.1.3's container
       document). THE ACCESSOR SIDE IS THIS REALM, and that IS §7.2.2.4's "current settings object" wherever
       this body can run at all: a direct `window.frameElement` makes the two the same object, and a
       cross-document `otherW.frameElement` only reaches this getter through §7.2.3.5 step 3, which performs
       [[GetOwnProperty]] on W after IsPlatformObjectSameOrigin(W) already held — so the reading script and this
       realm are same origin-domain before step 5 is asked, and §7.1.1's relation is an equivalence within each
       of its two classes (both domains set and equal, or both unset and same origin), which carries the answer
       across. Outside that check the getter is not invoked: `frameElement` is not on §7.2.1's cross-origin
       list, so a cross-origin read is a SecurityError at the proxy and never a null from here. */
    parent = window_proxy_parent_navigable(ctx, nav);
    DCHECK(window_proxy_is(parent),
           "§7.2.2.4 found a navigable with a §7.3.1.3 CONTAINER and no parent navigable — every child "
           "navigable is nested in the one its container's node document belongs to, and the create writes "
           "both links in one step (core/frame/navigable.c)");
    same_origin_domain = window_proxy_same_origin_domain_of(ctx, parent);
    JS_FreeValue(ctx, parent);
    if (!same_origin_domain) {
        JS_FreeValue(ctx, container);
        return JS_NULL;
    }
    return container;   /* Step 6 */
}

/* §7.2.2.4's BROWSING-CONTEXT LINKS — see window_proxy.h. The mapping between a window's two spellings
 * (another navigable is its PROXY, this one is the global) is window_proxy.c's `win_or_proxy`, applied by
 * every member that can answer with a navigable.
 *
 * §7.2.2's NAVIGABLE-SCOPED MEMBERS ARE ANSWERED ABOUT THEIR RECEIVER, and every one below reads it through
 * the one place that resolves it: Web IDL §3.7.6 Attributes' `jsValue` — "the this value, if it is not null or
 * undefined, or realm's global object otherwise" — mapped onto the navigable it names (§3.7.7 Operations says
 * the same for `close()`). Each of these used to carry `(void)this_val;` and `document_window_proxy(ctx)`,
 * which is that rule's MISSING-RECEIVER arm applied to every receiver: one member answering for whichever
 * navigable its realm has, whoever asks. HTML §7.2.3.5 step 3 is what makes that ordinary rather than exotic —
 * a same-origin WindowProxy performs [[GetOwnProperty]] ON W, so the getter a cross-document read invokes is
 * the OTHER document's, with the proxy as its receiver. */
static JSValue js_win_parent(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    return window_proxy_parent(ctx, nav);
}

static JSValue js_win_top(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    return window_proxy_top_of(ctx, nav);
}

static JSValue js_win_opener(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    return window_proxy_opener(ctx, nav);
}

/* §7.2.2.4's `opener` SETTER, and it has TWO branches that do different things — which is why it is written out
   here rather than declared [Replaceable].
     null  -> DISOWN: the navigable's opener link is severed, and NO own property is defined. A page writes this
              to cut a popup loose from the document that opened it, and defining an own `null` instead would
              answer null to the page while leaving the link intact for everything that reads the navigable.
     other -> §7.2.2.4's own step 2, "perform ? DefinePropertyOrThrow(this, "opener", { [[Value]]: the given
              value, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true })" — the descriptor
              [Replaceable]'s CreateDataPropertyOrThrow builds, reached through the same one implementation.
   THE TWO BRANCHES NAME TWO DIFFERENT THINGS AND §3.7.6 RESOLVES THE RECEIVER ONCE FOR BOTH: the disown is
   about the NAVIGABLE, the define is about the OBJECT, and for a receiver that is null or undefined those are
   this realm's WindowProxy and this realm's GLOBAL. Handing the raw `this_val` to the define wrote the page's
   value onto `undefined` — the same half-applied receiver rule that had the WindowProxy's own spelling of this
   member answering `undefined` where §7.2.2.4 answers `null`. */
static JSValue js_win_set_opener(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);
    JSValue js;
    int r;

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    if (JS_IsNull(val)) {
        window_proxy_disown_opener(ctx, nav);
        return JS_UNDEFINED;
    }
    js = window_proxy_this_object(ctx, this_val);
    r = idl_replace_with_value(ctx, js, "opener", val);
    JS_FreeValue(ctx, js);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* §7.2.2.1's `name` — THE NAVIGABLE'S, which is the same attribute `w.name` reads through the WindowProxy and is
   answered from the same record. It was a second source here: a source-only concolic with no example, so
   `open(url, "chan42")` gave "chan42" to the opener and an example-free unknown to the popup's own script,
   which is the popup unable to learn the name it was created with. §7.2.2.1 says "return this's navigable's
   target name"; window_proxy_name_value is where that is computed, including whether it is known at all. */
static JSValue js_win_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    return window_proxy_name_value(ctx, nav);
}

/* §7.2.2.1's `name` is SETTABLE, and it was not — the accessor had no setter at all, so `window.name = "x"` was a
   silent no-op and a page that names itself to be reached by `open(url, "x")` could not. It renames the
   NAVIGABLE, which is the same write `w.name = "x"` performs from outside.
   §7.2.2.1's `attribute DOMString name`, and the DOMString is the DECLARATION'S. Written as a bare
   JS_NewCFunction setter, the ToString ran from C — `window.name = {toString(){ for(;;){} }}` is the page's
   code in an activation with no flow base, so the loop drove to completion instead of parking, and the same
   value as a Proxy reached its `get` trap there too. The body now receives a real string, which is also what
   window_proxy_name_assign's own concolic DCHECK is written against. */
static JSValue js_win_set_name(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;
    return window_proxy_name_assign(ctx, nav, val);
}

/* §7.2.2.2's EXOTIC OWN-PROPERTY BEHAVIOUR — `window[0]`, and why it is not a property anyone sets.
 *
 * The global is a LEGACY PLATFORM OBJECT. Its SUPPORTED PROPERTY INDICES are its document-tree child
 * navigables, and `window[i]` is the i-th one's WindowProxy. Materialising those as own data properties would
 * be right until the first frame was appended, removed or moved — the set changes on every tree mutation, and
 * a count or a slot that a mutation forgot to adjust is wrong in exactly the case the spec files test. Exotic
 * behaviour is what §7.2.2.2 describes and what this is: every answer is computed from the tree at the moment
 * it is asked.
 *
 * THE THREE OPERATIONS ARE NOT SYMMETRIC, and each asymmetry is a spec sentence:
 *   [[GetOwnProperty]] answers only for a SUPPORTED index — an unsupported one is an ordinary miss, so
 *     `window[7] = "x"` on a frameless page really does create an ordinary property.
 *   [[DefineOwnProperty]] returns FALSE for EVERY array index, supported or not. `Object.defineProperty(window,
 *     0, …)` fails on a page with no frames at all, which is what distinguishes it from the above.
 *   [[Delete]] returns false only for a SUPPORTED index; anything else is the ordinary "nothing to delete".
 *
 * IT IS REACHED ONLY AFTER THE ORDINARY LOOKUP MISSES (quickjs consults a class's exotic get_own_property when
 * find_own_property found nothing), so a global variable read pays for this only when it was going to fail
 * anyway — and the index test is the engine's own, not a re-parse of the atom's text. */
static JSClassID g_window_class;

/* "IS A Window OBJECT" — the brand, off the class the global carries. DOM §2.9 step 6.9.5 asks it of every
   parent the event path walk reaches, because a Window is the one path entry that is NOT a node and so is the
   one the shadow-including ancestor test cannot answer for. Asking it as "is it not a node" would be an
   inference about who else can appear in a path rather than a fact about this object, and the class is what
   makes it a fact: HTML's global IS the Window, and window_install gives the global exactly this class. */
bool window_is(JSValueConst v)
{
    return g_window_class != 0 && JS_GetClassID(v) == g_window_class;
}

/* The child navigable this index names, or false. Owned on true. */
static bool win_supported_index(JSContext *ctx, JSAtom prop, JSValue *out)
{
    uint32_t idx;
    JSValue nav;

    if (!JS_AtomIsIndex(ctx, &idx, prop) || idx > (uint32_t)INT32_MAX) return false;
    nav = iframe_child_navigable(ctx, (int)idx);
    if (JS_IsUndefined(nav)) return false;
    *out = nav;
    return true;
}

static int win_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    JSValue v;
    (void)obj;
    if (!win_supported_index(ctx, prop, &v)) return 0;
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    /* §7.2.2.2: { [[Value]]: the child's WindowProxy, [[Writable]]: false, [[Enumerable]]: true,
       [[Configurable]]: true }. */
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = v;
    return 1;
}

static int win_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                          JSValueConst getter, JSValueConst setter, int flags)
{
    uint32_t idx;

    if (JS_AtomIsIndex(ctx, &idx, prop)) {
        if (flags & JS_PROP_THROW) {
            JS_ThrowTypeError(ctx, "cannot define an indexed property on the window object");
            return -1;
        }
        return 0;
    }
    /* EVERYTHING ELSE IS ORDINARY, and the exotic hook REPLACES the ordinary path rather than preceding it —
       so the ordinary path is re-entered here explicitly, with the exotic step suppressed. Forgetting this
       would not break `window[0]`; it would break every `var` and every property a page defines on the
       global. */
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

static int win_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSValue v;
    (void)obj;
    /* Reached only when the ordinary own-property scan found nothing, so "not an index" is "nothing to
       delete", which is true. */
    if (!win_supported_index(ctx, prop, &v)) return 1;
    JS_FreeValue(ctx, v);
    return 0;
}

static int win_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    int n = iframe_child_navigable_count(ctx), i;
    JSPropertyEnum *tab;

    (void)obj;
    /* §7.2.2.2 [[OwnPropertyKeys]] lists the supported indices FIRST, in order. quickjs merges what this
       returns with the object's ordinary keys, so only the indices belong here. */
    tab = n ? js_malloc(ctx, sizeof(*tab) * (size_t)n) : NULL;
    if (n && !tab) return -1;
    for (i = 0; i < n; i++) {
        tab[i].is_enumerable = true;
        tab[i].atom = JS_NewAtomUInt32(ctx, (uint32_t)i);
    }
    *ptab = tab;
    *plen = (uint32_t)n;
    return 0;
}

static const JSClassExoticMethods WINDOW_EXOTIC = {
    .get_own_property = win_get_own,
    .get_own_property_names = win_own_names,
    .delete_property = win_delete,
    .define_own_property = win_define_own,
    /* The lookup is a walk of the document tree and a read of each iframe's navigable slot — no page code, by
       construction, which is what lets the engine's own accessor walks run it from C. */
    .get_own_property_no_user_code = true,
};

static const JSClassDef WINDOW_CLASS = { "Window", NULL, NULL, NULL, &WINDOW_EXOTIC };

/* §7.2.2's INTERFACE PROTOTYPE OBJECT, and the chain it sits in: window -> Window.prototype ->
   EventTarget.prototype -> Object.prototype. The global had Object.prototype directly, so `Window` did not
   exist as a name, `window instanceof EventTarget` was false, and every member of Window was an OWN property
   of the global — which is where Web IDL puts only the [LegacyUnforgeable] ones (`window`, `self`, `location`,
   `top`, `document`). Everything else is declared on the prototype, and a page reads the difference: an
   `assert_own_property` on the wrong object, a descriptor test, a `delete window.closed`.
   §7.2.2's WindowProperties object sits between Window.prototype and EventTarget.prototype in the real chain
   and is what NAMED access (`window.myIframeName`) is declared on. It is not built here, so it is not claimed
   here either: this chain is two links of the three, and the third arrives with named access. */

/* HTML §7.3.3 — NAMED ACCESS ON THE WINDOW OBJECT, and the object it is declared on.
 *
 * `window.myFrameName` and `window.someElementId` are not properties of the global and not properties of
 * Window.prototype: Web IDL puts an interface's NAMED PROPERTIES on a separate object one link further up the
 * chain — window -> Window.prototype -> WindowProperties -> EventTarget.prototype — precisely so that a page's
 * own `window.foo = 1` SHADOWS the named property rather than colliding with it. The corpus walks that chain
 * link by link, so the object is not a formality: without it the chain is short by one and every level below
 * compares against the wrong prototype.
 *
 * THE ORDER IN §7.3.3 IS THE WHOLE ALGORITHM, and each branch is a different kind of answer:
 *   a document-tree child NAVIGABLE with that name wins outright, and answers with its WindowProxy;
 *   a single named ELEMENT that is itself a container with a navigable answers with THAT navigable's proxy —
 *     `window.myIframeName` is the frame's window, not the <iframe> element;
 *   a single named element answers with the element;
 *   more than one answers with a live HTMLCollection, because a page reads `.length` off it.
 * A "named element" is two rules, not one: any HTML element whose `id` matches, plus embed/form/img/object/
 * iframe whose `name` attribute matches — see collections.c, which owns the filter.
 *
 * [LegacyUnenumerableNamedProperties] is what Window carries, so the descriptor is
 * { writable: true, enumerable: FALSE, configurable: true }. */

/* The child navigable whose browsing-context name is `name`, or JS_UNDEFINED. Owned on success. */
static JSValue win_named_navigable(JSContext *ctx, const char *name)
{
    int n = iframe_child_navigable_count(ctx), i;

    for (i = 0; i < n; i++) {
        JSValue nav = iframe_child_navigable(ctx, i);
        if (JS_IsUndefined(nav)) continue;
        if (!strcmp(window_proxy_name(nav), name)) return nav;
        JS_FreeValue(ctx, nav);
    }
    return JS_UNDEFINED;
}

/* §7.3.3's value for `name`, or JS_UNDEFINED when the name is not supported. Owned on success. */
static JSValue win_named_value(JSContext *ctx, const char *name)
{
    JSValue coll, doc, first, second, out = JS_UNDEFINED;

    if (!*name) return JS_UNDEFINED;   /* the empty name is no name at all, and no attribute carries it */
    out = win_named_navigable(ctx, name);
    if (!JS_IsUndefined(out)) return out;
    if (!document_root_node(ctx)) return JS_UNDEFINED;

    doc = node_wrap(ctx, document_root_node(ctx));
    coll = collections_named(ctx, doc, name);
    JS_FreeValue(ctx, doc);
    if (JS_IsException(coll)) return JS_UNDEFINED;
    /* HOW MANY, asked by INDEX rather than by `length`. An HTMLCollection's `length` is an IDL accessor, and
       reaching a getter from C is the one thing this engine refuses — there is no flow base under a C
       activation. The indexed read is the collection's exotic own-property behaviour, which runs no page code
       and is declared so; and the two indices are all this branch needs, because §7.3.3 only distinguishes
       none, exactly one, and more than one. */
    first  = JS_GetPropertyUint32(ctx, coll, 0);
    second = JS_GetPropertyUint32(ctx, coll, 1);
    if (JS_IsUndefined(first)) {
        JS_FreeValue(ctx, first); JS_FreeValue(ctx, second); JS_FreeValue(ctx, coll);
        return JS_UNDEFINED;
    }
    if (!JS_IsUndefined(second)) {
        JS_FreeValue(ctx, first); JS_FreeValue(ctx, second);
        return coll;   /* the LIVE collection is the answer, not a snapshot of it */
    }
    JS_FreeValue(ctx, second);
    out = first;
    JS_FreeValue(ctx, coll);
    /* §7.3.3: a single named element that HAS a content navigable answers with the navigable's WindowProxy —
       `window.myIframeName` is the frame's window, and a page that then reads `.document` off it would get an
       element otherwise. */
    /* ASKED OF THE COMPONENT THAT OWNS THE SLOT, never read off `contentWindow`: that attribute is an IDL
       accessor, and this walk runs from C with no flow base under it — which is why the exotic declares
       `get_own_property_no_user_code`. Reading it aborted window-properties, window-named-properties and
       nested-context, each on the first named frame it reached. */
    {
        JSValue nav = iframe_navigable(ctx, out);
        if (JS_IsObject(nav)) { JS_FreeValue(ctx, out); return nav; }
        JS_FreeValue(ctx, nav);
    }
    return out;
}

static int win_named_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const char *name;
    JSValue v;

    (void)obj;
    /* A SYMBOL IS NOT A NAMED PROPERTY. Web IDL's named-property algorithm applies to STRING property names
       only, and stringifying a symbol here would answer for `Symbol.toStringTag` with whatever element happened
       to carry the id "Symbol(Symbol.toStringTag)" — and, far more often, would run a whole tree walk for every
       symbol lookup that reaches this object. */
    {
        JSValue pv = JS_AtomToValue(ctx, prop);
        bool sym = JS_IsSymbol(pv);
        JS_FreeValue(ctx, pv);
        if (sym) return 0;
    }
    name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    v = win_named_value(ctx, name);
    JS_FreeCString(ctx, name);
    if (JS_IsUndefined(v)) return 0;
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    desc->flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;   /* [LegacyUnenumerableNamedProperties] */
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = v;
    return 1;
}

static const JSClassExoticMethods WINDOW_PROPS_EXOTIC = {
    .get_own_property = win_named_get_own,
    /* The lookup is a walk of the document tree and a read of content attributes — no page code, by
       construction, which is what lets the engine's own accessor walks run it from C. */
    .get_own_property_no_user_code = true,
};
static const JSClassDef WINDOW_PROPS_CLASS = { "WindowProperties", NULL, NULL, NULL, &WINDOW_PROPS_EXOTIC };
static JSClassID g_window_props_class;

/* THE SAME FOUR TOUCH HANDLERS core/html/html_element.c excludes, and for the same reason — this interface
   includes the same `GlobalEventHandlers`, so §Touch Events Level 2's "this mixin must not be implemented"
   reaches it too. The list is stated HERE rather than shared from there because idl_members_excluded reads the
   interface name as a literal at the call and resolves its table per file; one list named from three sites is
   not expressible, and html_element.c says so where it declares its own. */
static const char *const TOUCH_EXCLUDED[] = { "ontouchstart", "ontouchend", "ontouchmove", "ontouchcancel" };


/* THE CLASSES ARE THE AGENT'S, THE PROTOTYPES ARE THE REALM'S — and that line is Web IDL's, not a convenience.
   A class id is a registration in the JSRuntime and there is one runtime per agent; a PROTOTYPE is an object,
   and §3.7 gives every realm its own, which is why `frames[0].Window.prototype !== Window.prototype` in a
   browser. So this registers, and window_install builds. */
static int g_id_close, g_id_blur, g_id_stop;   /* declared once per agent — see window_init */
static int g_id_opener_set;   /* §7.2.2.4's `opener` setter, declared with them for the same reason */
static int g_id_name_set;     /* §7.2.2.1's `name` setter — its DOMString conversion is the page's code */

void window_init(JSContext *ctx)
{
    DCHECK(g_window_class == 0, "window_init ran twice — a class is registered once per agent, and a second "
                                "registration would give one interface two ids that compare unequal");
    JS_NewClassID(JS_GetRuntime(ctx), &g_window_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_window_class, &WINDOW_CLASS) == 0,
          "the Window class could not be registered");
    JS_NewClassID(JS_GetRuntime(ctx), &g_window_props_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_window_props_class, &WINDOW_PROPS_CLASS) == 0,
          "the WindowProperties class could not be registered");
    /* THE MEMBERS ARE DECLARED HERE AND INSTALLED PER REALM. A declaration builds a pool entry and a member has
       ONE, so declaring inside the install would mint a second entry for the second realm's prototype — which
       is the same shape as a per-wrapper mint and is what the pool's seal asserts against. */
    bar_prop_init(ctx);   /* §7.2.2.5's BarProp class, one per agent */
    g_id_opener_set = idl_setter_id(ctx, IDL_ANY, false, js_win_set_opener, 0);
    g_id_name_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_win_set_name, 0);
    g_id_close = idl_method_id(ctx, NULL, 0, js_win_close, 0);
    g_id_blur  = idl_method_id(ctx, NULL, 0, js_win_noeffect, 1);
    g_id_stop  = idl_method_id(ctx, NULL, 0, js_win_noeffect, 2);
}

void window_install(JSContext *ctx, JSValueConst global, const char *url)
{
    JSValue g = (JSValue)global, gp, props, etp;

    DCHECK(JS_IsObject(global), "window_install was given something that is not the global object");
    DCHECK(g_window_class != 0, "window_install ran before window_init registered the Window class");

    /* §7.2.2.2's exotic behaviour comes from the object's CLASS, and the global was created by the context
       before any host class existed — so it is given one here. It owns no per-object data (no finalizer, no
       gc_mark), which is what makes handing it to an already-built object sound. */
    CHECK(JS_SetGlobalClass(ctx, g_window_class) == 0,
          "the global object would not take the Window class — §7.2.2.2's indexed access has nowhere to live");

    /* THE PROTOTYPE CHAIN, before any member is installed on any of its objects. PER REALM, and held by
       quickjs's own per-context class-prototype slot rather than by a static in this file: a second same-origin
       document is a second realm in this agent, and a static would have given both the first realm's Window
       objects — so `frames[0].Window` would have been this document's. */
    etp = event_target_proto(ctx);   /* THIS realm's — §3.6's [Global] rule is read off the member's own ctx */
    props = JS_NewObjectProtoClass(ctx, etp, g_window_props_class);
    JS_FreeValue(ctx, etp);
    CHECK(!JS_IsException(props), "the WindowProperties object could not be allocated");
    idl_interface_tag(ctx, props, "WindowProperties");
    gp = JS_NewObjectProto(ctx, props);
    CHECK(!JS_IsException(gp), "Window.prototype could not be allocated");
    idl_interface_tag(ctx, gp, "Window");
    JS_SetClassProto(ctx, g_window_props_class, props);   /* the realm owns them from here */
    JS_SetClassProto(ctx, g_window_class, JS_DupValue(ctx, gp));
    JS_SetPrototype(ctx, g, gp);
    /* ECMAScript gives THE GLOBAL OBJECT an own @@toStringTag of "global", and it shadows the interface tag
       that §3.7.3 puts on Window.prototype — so `Object.prototype.toString.call(window)` answered
       "[object global]" where every browser answers "[object Window]". That own property is the plain-host
       global's, not a Window's: HTML's global IS the Window, and the tag it carries is the interface's. */
    JS_DeleteProperty(ctx, g, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG), 0);
    event_target_install_interface(ctx, g);   /* §2.7's interface object, now that its prototype is in a chain */
    JS_SetPropertyStr(ctx, g, "Window", idl_interface_object(ctx, "Window", gp));

    /* 7.2.2: window, self and frames all return THIS Window's proxy, and the global object IS that proxy here —
       so `window.X`, `self.X` and a bare `X` are one read spelled three ways. */
    /* EVERY MEMBER BELOW IS AN OWN PROPERTY OF THE GLOBAL, because Window is declared [Global] — Web IDL
       §3.7.3: an interface with [Global] defines its members on the GLOBAL OBJECT, not on the interface
       prototype object, which is left with nothing on it but its @@toStringTag and `constructor`. That is not
       a placement detail: `Object.getOwnPropertyDescriptor(window, "opener")` is `undefined` when the member
       is one link up the chain, and window-properties.https.html reads exactly that for every attribute and
       every method Window has.
       The comment that stood here said the opposite — that [LegacyUnforgeable] is what puts a member on the
       global and that `frames`, `parent` and `opener` therefore "are declared on the prototype like every
       other member". [LegacyUnforgeable] decides the ATTRIBUTES (non-configurable, so a page cannot shadow or
       delete), never the LOCATION; on a [Global] interface there is no other location. */
    /* §3.7.6 makes it an ACCESSOR — every attribute is one, and [LegacyUnforgeable] decides only that it is
       not configurable. It was a data property, which is the right VALUE behind the wrong kind of property:
       `Object.getOwnPropertyDescriptor(window, "window").get` is a function in every browser and was undefined
       here. */
    idl_install_value_attribute(ctx, g, "window", JS_DupValue(ctx, global), IDL_ATTR_UNFORGEABLE);
    /* `self` is [Replaceable], not [LegacyUnforgeable] — `window` is the unforgeable one. A page may
       overwrite `self` and the IDL says so; a fixed own value said it could not. */
    idl_install_replaceable_value(ctx, g, "self", JS_DupValue(ctx, global));
    /* §7.2.2.4 marks `top` [LegacyUnforgeable] — an OWN property — but its VALUE is the navigable's, and `top`
       is a WALK of the parent chain, so a grandchild answers with the top-level traversable rather than with
       itself. An own ACCESSOR, not an own value frozen at install time. */
    {
        /* THE ATOM IS BORROWED BY THE DEFINE, NOT CONSUMED BY IT. JS_DefinePropertyGetSet frees the getter and
           the setter it is handed and leaves `prop` alone, so an atom minted inline into the argument list has
           nobody to give it back — `top` was named by JS_FreeRuntime's atom walk on 118 files of `css/cssom`.
           Every other JS_DefinePropertyGetSet in this engine (abort.c, idl_args.c, html_element.c) already
           holds the atom in a local and frees it; this was the one call site that did not. */
        JSAtom a = JS_NewAtom(ctx, "top");
        CHECK(a != JS_ATOM_NULL, "§7.2.2.4's `top` could not be interned");
        JS_DefinePropertyGetSet(ctx, g, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_win_top, "get top", 0,
                                                     JS_CFUNC_getter_magic, 0),
                                JS_UNDEFINED, JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    idl_install_replaceable_value(ctx, g, "frames", JS_DupValue(ctx, global));   /* [Replaceable] */
    /* §7.2.2.4's `parent` and `opener` ARE THE NAVIGABLE'S, so they are read from this realm's own WindowProxy
       rather than answered here. They were two FIXED values behind two comments explaining why an embedder
       could not exist — "no embedder is reachable from this instance" and "the document was navigated to, not
       opened by a script in another navigable" — and both were true exactly while one instance was one
       document. A §7.4 child realm in this agent HAS a creator, and a popup whose `opener` is null cannot post
       back to the page that opened it, which is the whole of what a popup is for. */
    idl_install_replaceable(ctx, g, "parent", js_win_parent, 0);   /* [Replaceable] readonly */
    /* `opener` is NOT [Replaceable]: the IDL declares `attribute any opener`, and §7.2.2.4 gives it setter steps
       of its own whose null branch DISOWNS rather than assigns. Its non-null branch is [Replaceable]'s define,
       reached through the same implementation. */
    idl_install_accessor(ctx, g, "opener", js_win_opener, 0, g_id_opener_set);
    /* §7.2.2.5's six user-interface bars. */
    bar_prop_install(ctx, g);

    /* §7.2.2.4 `frameElement` — the element this navigable is nested THROUGH. */
    idl_install_accessor(ctx, g, "frameElement", js_win_frame_element, 0, -1);

    /* `closed` is a GETTER over the NAVIGABLE's per-flow state, because close() changes it. */
    idl_install_accessor(ctx, g, "closed", js_win_closed, 0, -1);
    idl_install_replaceable(ctx, g, "length", js_win_length, 0);   /* [Replaceable] readonly */
    idl_install_method(ctx, g, "close", 0, g_id_close);
    /* §6.6.6's `Window.focus()` — its OWN algorithm, installed by the component that owns §6.6.4's steps. */
    focus_install_window_members(ctx, g);
    /* Selection API §4.2's `Selection? getSelection()`, installed by the component that owns it for the same
       reason. §4.2 defines it as §4.1's member invoked on `this's Window.document`, so it is not a second
       algorithm and the two cannot answer differently. */
    selection_install_window_members(ctx, g);
    idl_install_method(ctx, g, "blur",  0, g_id_blur);
    idl_install_method(ctx, g, "stop",  0, g_id_stop);

    /* The Window's origin, serialized — the principal, concrete for the same reason Location's is: a bundle
       compares it and builds URLs out of it, and a shape there loses every endpoint behind the comparison.
       IT IS §4.7's SERIALIZATION, not a substring of the address. The scan that stood here took everything
       before the first `/?#` after `://`, which is not an origin: it kept a default port that §4.7 drops, kept
       userinfo that an origin never has, and had no answer at all for a scheme with an OPAQUE origin — a
       `data:` document's `origin` is the string "null", and this gave it nothing. */
    if (url && *url) {
        UrlRecord rec;
        if (url_parse(&rec, url, strlen(url), NULL)) {
            char *origin = url_serialize_origin(&rec);
            idl_install_replaceable_value(ctx, g, "origin", JS_NewString(ctx, origin));
            free(origin);
        }
        url_record_free(&rec);
    }

    /* HTML §8.1.7.1's other WindowOrWorkerGlobalScope answer about this environment, beside `origin` because
       the two are the pair a page reads together — §8.1.7.1's own note tells developers to prefer `self.origin`
       over `location.origin` for exactly the reason this one exists: they are facts about the ENVIRONMENT and
       not about whatever URL the Document happens to be showing. */
    idl_install_accessor(ctx, g, "isSecureContext", js_win_is_secure_context, 0, -1);
    /* §7.1.2's `originAgentCluster` and §8.1.7.1's `crossOriginIsolated` — two answers about THIS AGENT'S
       CLUSTER, installed by the component that computes it (core/frame/agent_cluster.c) rather than written out
       here as two booleans, because §7.1.1.2's `document.domain` setter and HR-TIME §4's clock resolution read
       the same §7.1.4 mode, and one fact answered from four places is four places for it to drift. */
    agent_cluster_install(ctx, g);

    idl_install_accessor(ctx, g, "name", js_win_get_name, 0, g_id_name_set);
    /* HTML §8.1.7.2: Window includes GlobalEventHandlers AND WindowEventHandlers, so `window.onload`,
       `onerror`, `onmessage` and the rest are THIS interface's members and belong to this install — the same
       reason §2.7's interface object is installed above. They were a separate line in each host's per-document
       list, which is how the WPT runner came to have none of them: every unqualified `onload = f` in the
       corpus wrote a plain own property that nothing ever fired. A member of Window is installed by Window. */
    event_target_install_handlers(ctx, g, EH_GLOBAL | EH_WINDOW);
    idl_members_excluded(ctx, g, "Window", TOUCH_EXCLUDED,
                         (int)(sizeof(TOUCH_EXCLUDED) / sizeof(TOUCH_EXCLUDED[0])),
                         "Touch Events Level 2, `Extensions to the GlobalEventHandlers mixin`: \"For user "
                         "agents where expose legacy touch event APIs is false, this mixin must not be "
                         "implemented.\" This agent's `expose legacy touch event APIs` is false — TouchEvent, "
                         "Touch and TouchList are absent, so a touch handler would have nothing to be handed");
    JS_FreeValue(ctx, gp);   /* the realm's class-proto slot and the chain hold it now */
}

void window_free(JSContext *ctx)
{
    (void)ctx;
    g_window_class = 0;
    g_window_props_class = 0;
    bar_prop_free(ctx);
}
