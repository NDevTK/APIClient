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
#include "core/agent_state.h"
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
#include "core/realm.h"
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

/* HTML §7.2.2.1 "Opening and closing windows"' `stop()`, WHICH HAS REAL STEPS AND IS NOT §6.6.6's NO-EFFECT.
 * The standard states two: "If this's navigable is null, then return. Stop loading this's navigable." Step 1 is
 * written below because this engine can answer it; step 2 is the capability that does not exist, and it now
 * says so at the site instead of being served by `blur`'s body — where a page calling `window.stop()` got the
 * exact bytes the spec prescribes for a member with no steps at all, and nothing anywhere could tell the two
 * apart.
 *
 * WHAT STEP 2 IS. §7.5.11 "Aborting a document load" defines "stop loading a navigable": (1) let document be
 * navigable's active document; (2) if document's unload counter is 0 AND navigable's ongoing navigation is a
 * navigation ID, then set the ongoing navigation for navigable to null — whose note is the observable half,
 * "this will have the effect of aborting any ongoing navigations of navigable"; (3) abort a document and its
 * descendants given document, which queues a global task on the navigation and traversal task source per
 * descendant navigable and then aborts the document — cancelling every instance of the fetch algorithm in its
 * context and aborting its active parser.
 *
 * WHAT TO BUILD, AND WHY IT IS NOT THIS FILE'S. The field step 2 reads is declared by HTML §7.4.2.5 "Aborting
 * navigation" — "each navigable has an ongoing navigation, which is a navigation ID, "traversal", or null,
 * initially null" — together with its "set the ongoing navigation" operation, which informs the navigation API
 * about aborting navigation before it writes. core/frame/navigable.c exposes neither the field nor a stop, so
 * the member cannot ask its question, let alone answer it. THAT DIFF ALREADY HAS TWO OTHER CALLERS WAITING AND
 * MUST DELETE ALL THREE SITES AT ONCE: core/frame/session_history.c's §7.4.6.2 step 5.1 is the WRITER (its
 * comment declines to write a field with no reader and names this body as the reason), and
 * core/html/document_open.c's §8.4.1 step 8 is the second READER ("if document's node navigable's ongoing
 * navigation is a navigation ID, then stop loading document's node navigable"). Step 3's own two effects need
 * state this engine does not keep either — core/frame/document_lifecycle.c argues there is no `salvageable`
 * field because every reader of it is on a path where it is already false, and core/html/document_open.c's step
 * 7 records that there is no mid-parse state to abort — so the abort arrives with the parse that outlives its
 * own C call. Nothing here is approximated in the meantime: the approximations available (treating a parked
 * host request as the navigation, or asking whether THIS flow is the one navigating) are both about the FLOW
 * and the question is about the NAVIGABLE, which is the same reason document_open.c gives for naming it rather
 * than guessing. */
static JSValue js_win_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)argv; (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* Web IDL §3.7.7's TypeError, already thrown */
    DCHECK(argc == 0,
           "HTML §7.2.2.1 Opening and closing windows declares `undefined stop()` with no arguments and this "
           "body was reached with one — the declaration in window_init and the IDL have come apart");
    (void)argc;
    /* STEP 1: "If this's navigable is null, then return." The one question §7.2.2.4's `top`, `parent` and
       `frameElement` already ask, asked in the one place (window_proxy.h) — a navigable severed by §7.3.1.6
       step 3's destroy-a-child-navigable, or whose Document §7.5.10 destroyed. */
    if (window_proxy_navigable_null(ctx, nav)) return JS_UNDEFINED;
    /* STEP 2. */
    DFAIL("HTML §7.2.2.1 Opening and closing windows' `stop()` step 2 is STOP LOADING this's navigable, and "
          "this engine has no entry for it. BUILD HTML §7.4.2.5 Aborting navigation's `ongoing navigation` on "
          "the navigable (a navigation ID, \"traversal\", or null, initially null) and its `set the ongoing "
          "navigation` operation in core/frame/navigable.c, then §7.5.11 Aborting a document load's `stop "
          "loading a navigable` over it. That diff must land ALL THREE SITES TOGETHER, because the field's one "
          "writer and its other reader are already written as absences waiting for it: "
          "core/frame/session_history.c's §7.4.6.2 step 5.1 writes it, and core/html/document_open.c's §8.4.1 "
          "step 8 reads it. Step 3's `abort a document and its descendants` additionally needs the mid-parse "
          "state core/html/document_open.c step 7 records as absent, and the `salvageable` field "
          "core/frame/document_lifecycle.c declines to keep. Until then this member must CRASH rather than "
          "share §6.6.6's `blur` no-effect body, which is what made an unbuilt capability read as the "
          "standard's own do-nothing");
    return JS_UNDEFINED;   /* the release fall-through: DFAIL is compiled out, and there is nothing to run */
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
    /* Steps 1-2. Step 1 is "let current be this's node navigable", and §7.3.1 Navigables defines that as
       "the navigable whose active document is node's node document, or null if there is no such navigable" —
       which has no answer once the navigable has been detached from the tree or its Document destroyed —
       one question, asked where `top` and `parent`
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

/* HTML §7.2.2.5 "Historical browser interface element APIs" — `attribute DOMString status`, whose entire
 * definition is one sentence: "For historical reasons, the status attribute on the Window object must, on
 * getting, return the last string it was set to, and on setting, must set itself to the new value. When the
 * Window object is created, the attribute must be set to the empty string. It does not do anything else."
 *
 * "IT DOES NOT DO ANYTHING ELSE" IS NOT A LICENCE TO OMIT IT. The spec COMPUTES a real value here — the empty
 * string at creation, the last assignment afterwards — so the thing that is absent is a status BAR, not an
 * answer, and §NO STUBS draws its line at exactly that: a member is honestly absent only where the spec
 * defines no scriptable headless result. A missing member on the global is `undefined`, not a throw, and
 * `undefined` is what document-domain-removed-iframe.html read on ALL FOUR of its subtests — including the
 * control that removes nothing and sets no `document.domain`, which is how a member this cheap came to be
 * invisible behind a file whose name says it is about something else entirely.
 *
 * IT IS THE WINDOW'S, WHICH IS WHY IT IS THE REALM'S RECORD AND NOT THE NAVIGABLE'S. `name` and `closed` above
 * are read off the navigable because they SURVIVE a navigation — whoever opened the context named it and the
 * name outlives the document. This is the opposite statement: it is reset to "" every time a Window object is
 * created, so it belongs to the realm that Window is the global of, and a navigation gets a new one because a
 * navigation gets a new realm. Reading it off the navigable would carry one document's status into the next.
 *
 * AND THAT IS WHAT MAKES ITS TEST A REGRESSION GUARD FOR §7.2.3 RATHER THAN A TEST OF THIS MEMBER.
 * `contentWindow.status` is read on the line AFTER `iframe.remove()` and must still answer, because §7.2.3's
 * internal methods are performed on W — the [[Window]] internal slot — and a Window OUTLIVES its browsing
 * context; the file's own comment cites crbug.com/1095145, the browser bug where those properties went
 * undefined. window_proxy.c's split between `wp_bc_null` (the sever, which `closed` and `opener` take) and
 * `wp_closed` (this heap holds no active document, which the §7.2.3 forward takes) is what keeps that forward
 * alive across the removal. That split had NOTHING READING IT through a member whose value survives the sever
 * and whose absence is not a throw, so it could have been folded back into one predicate with every subtest
 * still failing exactly as it already did. This is the member that makes it observable.
 *
 * THE RECORD IS A BASELINE JS OBJECT, unreachable from the page, which is §3.1.5's readiness record and
 * §6.4.1's activation timestamps in the same shape and for the same two reasons: nothing but this component
 * can write it, and the assignment is an ORDINARY PROPERTY WRITE that the heap COW captures — so one arm of a
 * fork sets a status its sibling never sees, and a parked flow's status parks with it. A malloc'd C string
 * would be a byte the delta cannot carry and the cold tier cannot serialize.
 *
 * §7.2.1 DOES NOT LIST IT, so a cross-origin `otherW.status` is a SecurityError and not `undefined` — that is
 * already true without a line here, because §7.2.3.5 steps 4-6 answer every name the fixed list omits and this
 * member is reached only through the same-origin forward that precedes them. */
static int g_status_slot = -1;

/* THIS REALM'S §7.2.2.5 RECORD. Owned — the caller frees. */
static JSValue win_status_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_status_slot);

    DCHECK(JS_IsObject(rec), "a realm answered for its Window's §7.2.2.5 status with no record — window_install "
                             "builds it with the realm so it is BASELINE, and a realm that reached a member "
                             "without running that install is one nothing put a Window in");
    return rec;
}

/* THE RECEIVER IS NOT WHAT THIS READS, AND THE ONE CASE WHERE THAT IS WRONG IS ASSERTED RATHER THAN GUESSED.
 * A C member runs in the realm that DEFINED it (js_call_c_function takes `ctx` from the function object), so
 * `ctx` here IS the Window whose status this is — which is exactly right for `window.status`, for a bare
 * `status`, and for `frame.contentWindow.status`, where §7.2.3's forward hands the read to the FRAME's own
 * global and therefore to the FRAME's own getter. It is also why the receiver reaching this may legitimately
 * be a WindowProxy rather than a Window: the forward performs OrdinaryGetOwnProperty on W and quickjs calls
 * the descriptor's getter with whatever object the read started from.
 * THE WRONG CASE IS A FOREIGN REALM'S Window, which same-origin documents make reachable — they are one heap,
 * so `Object.getOwnPropertyDescriptor(a, "status").get.call(b)` is a real expression — and the answer would be
 * A's status wearing B's identity. That is the same missing edge window_proxy_this_navigable's own DCHECK
 * names: a Window object to the thing it is the Window OF, without going through a realm. */
static void win_status_assert_receiver(JSContext *ctx, JSValueConst this_val)
{
#if APICLIENT_DEV
    JSValue g = JS_GetGlobalObject(ctx);
    bool foreign = window_is(this_val) && JS_VALUE_GET_PTR(g) != JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, g);
    DCHECK(!foreign, "§7.2.2.5's `status` was reached with ANOTHER realm's Window object as its receiver. The "
                     "value is the RECEIVER's Window's, and this member reads the realm the getter was minted "
                     "in — so the answer would be this document's status wearing that document's identity. "
                     "BUILD the Window -> realm edge that does not go through ctx (window_proxy_this_navigable "
                     "names the same missing edge) and read the record out of THAT realm");
#else
    (void)ctx; (void)this_val;
#endif
}

static JSValue js_win_get_status(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue rec, v;

    (void)magic;
    win_status_assert_receiver(ctx, this_val);
    rec = win_status_record(ctx);
    v = JS_GetPropertyStr(ctx, rec, "status");
    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsString(v), "§7.2.2.5's status record holds something that is not a string — it is born the "
                           "empty string and the only writer is a setter whose IDL_DOMSTRING conversion has "
                           "already run, so a non-string here is a second writer nobody declared");
    return v;
}

/* §7.2.2.5's setter, and the DOMString is the DECLARATION'S — `idl_setter_id(ctx, IDL_DOMSTRING, ...)` runs
   the conversion, so `window.status = {toString(){ for(;;){} }}` is the page's code under a flow base that can
   park, exactly as `name`'s is, and this body receives a real string. Written as a bare JS_NewCFunction setter
   the ToString would run from C, in an activation with no flow base, and the loop would drive to completion. */
static JSValue js_win_set_status(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue rec;

    (void)magic;
    win_status_assert_receiver(ctx, this_val);
    DCHECK(JS_IsString(val), "§7.2.2.5's status setter body was handed a value its IDL_DOMSTRING declaration "
                             "should already have converted");
    rec = win_status_record(ctx);
    JS_SetPropertyStr(ctx, rec, "status", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, rec);
    return JS_UNDEFINED;
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
/* THE RUNTIME THE TWO CLASSES AND THE FIVE POOL ENTRIES BELOW WERE DECLARED IN. It is what makes "this
   component is declared" a fact separate from any one of the values it declares — see window_is. */
static JSRuntime *g_window_rt;

/* "IS A Window OBJECT" — the brand, off the class the global carries. DOM §2.9 step 6.9.6 asks it of every
   parent the event path walk reaches, because a Window is the one path entry that is NOT a node and so is the
   one the shadow-including ancestor test cannot answer for. Asking it as "is it not a node" would be an
   inference about who else can appear in a path rather than a fact about this object, and the class is what
   makes it a fact: HTML's global IS the Window, and window_install gives the global exactly this class.
   IT OPENED `g_window_class != 0 &&`, WHICH IS TWO QUESTIONS SHARING ONE ANSWER — "this component is not
   declared" and "this object is not a Window" both came back `false`. That was harmless only while the id was
   carried past the release; now that window_free gives it back (core/agent_state.h), the folded predicate
   would report EVERY LIVE Window as something else at every branch site the moment the release column ran.
   The two are separated: the declaration is asserted, the brand is answered. Every call site is a page-visible
   algorithm — DOM §2.9's path walk through event_target.c's tree hook, §3.7.6's implements test in
   idl_args.c, window_proxy_this_navigable's receiver arm and remote_object.c's encoder — and not one of them
   is reachable from any release, which is what makes the assert an assert rather than a new failure mode. */
bool window_is(JSValueConst v)
{
    DCHECK(g_window_rt != NULL,
           "§7.2.2's Window brand was asked before window_init registered the class or after window_free gave "
           "it back — with no class there is no answer, and the `g_window_class != 0 &&` that used to stand "
           "here returned `not a Window` for an undeclared browser and for a live global alike");
    return JS_GetClassID(v) == g_window_class;
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
    /* HTML §7.2.3.5 [[GetOwnProperty]] ( P ): "Return PropertyDescriptor { [[Value]]: value, [[Writable]]:
       false, [[Enumerable]]: true, [[Configurable]]: true }." HTML §7.2.2.2 Indexed access on the Window
       object is where a reader looks for this and it is NOT where the descriptor is — its whole normative
       content on the subject is one sentence of delegation, "Indexed access to document-tree child navigables
       is defined through the [[GetOwnProperty]] internal method of the WindowProxy object." */
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
        /* THE SPEC'S ANSWER IS ONE WORD AND THE CALLERS MAKE FOUR OBSERVABLES OUT OF IT, so the word is spoken
           once, to the engine, instead of being decoded here. `flags & JS_PROP_THROW` decoded exactly two of
           the four: Object.defineProperty, which is ECMAScript §7.3.8 DefinePropertyOrThrow step 2 "If success
           is false, throw a TypeError exception", and Reflect.defineProperty, which is §28.1.3
           Reflect.defineProperty ( target, key, attrs ) step 4 "Return ? target.[[DefineOwnProperty]](
           propertyKey, propertyDesc)" and so reports the boolean. A STRICT-mode assignment arrives carrying
           JS_PROP_THROW_STRICT instead, which that test does not read, so ECMAScript §6.2.5.6 PutValue step
           3.e — "If succeeded is false and refRecord.[[Strict]] is true, throw a TypeError exception" — never
           fired, and `'use strict'; window[9] = "x"` on a page with fewer than ten child navigables completed
           silently. Resolving JS_PROP_THROW_STRICT needs the running
           frame's strictness, which lives inside quickjs, so JS_RefuseOrThrowTypeError is the only spelling of
           "return false" available to a hook compiled outside that file — and being the only one is what keeps
           the four observables from being re-derived, differently, in each class that grows a hook. */
        return JS_RefuseOrThrowTypeError(ctx, flags,
                                         "cannot define an indexed property on the window object");
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
    /* HTML §7.2.3.10 [[OwnPropertyKeys]] ( ) lists the supported indices FIRST, in order: "Let keys be the
       range 0 to maxProperties, exclusive." and then "return the concatenation of keys and
       OrdinaryOwnPropertyKeys(W)". quickjs merges what this returns with the object's ordinary keys, so only
       the indices belong here. (§7.2.2.2 Indexed access on the Window object states no [[OwnPropertyKeys]] at
       all — it delegates the whole of indexed access to the WindowProxy's internal methods.) */
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

/* THERE IS NO [[PreventExtensions]] HERE, AND THAT IS THE SPEC RATHER THAN THE OMISSION IT LOOKS LIKE beside
 * the tables in this engine that carry one. Web IDL §3.9.5 [[PreventExtensions]] — "Return false." — is a
 * LEGACY PLATFORM OBJECT's, and the definition that excludes this class by name is one section away, in Web
 * IDL §2.12 Objects implementing interfaces: "Legacy platform objects are platform objects that implement an
 * interface which does not have a [Global] extended attribute, and which supports indexed properties, named
 * properties, or both." (§3.9 itself never restates it, which is why the number to cite for the EXCLUSION is
 * not the number to cite for the refusal.) Window IS [Global], so it is not one; its indexed access is
 * §7.2.2.2's and its named access lives on the WindowProperties object below, which is a
 * named properties object and carries §3.7.4.5's refusal instead. HTML's refusal for a window is §7.2.3.4
 * [[PreventExtensions]] ( ) — also "Return false." — and that is the WINDOWPROXY's internal method, not this
 * object's. A Window's own extensibility is ordinary, so this table is complete for what this class IS.
 *
 * NAMED RESIDUAL — WHAT IS NOT COVERED: `Object.freeze(window)`, `Object.preventExtensions(window)` and
 * `Reflect.preventExtensions(window)` AT THE ROOT NAVIGABLE. A browser refuses all three at §7.2.3.4, because
 * `window` there names a WindowProxy. Here it does not: window_install gives the GLOBAL OBJECT this class and
 * installs `window`, `self` and `frames` as that same object, identifying the Window with its proxy — so there
 * is no WindowProxy at the root for §7.2.3.4 to be asked of, and the freeze succeeds. Every CHILD navigable is
 * already covered: `frames[0]`, `iframe.contentWindow`, `parent` and a grandchild's `top` are
 * core/frame/window_proxy.c's WindowProxy objects, whose table carries §7.2.3.4.
 * WHAT THE NEXT DIFF BUILDS: a WindowProxy for the ROOT navigable, so `window`, `self`, `frames` and `top` at
 * a top-level document resolve to one rather than to the global — which is what §7.2.2 The Window object says
 * they return, and what the identification above stands in for.
 * HOW ITS ABSENCE WOULD SHOW: in ONE run on a page with an iframe, `Object.freeze(window)` returns the object
 * and `Object.isExtensible(window)` is then false, while `Object.freeze(frames[0])` throws a TypeError and
 * `Object.isExtensible(frames[0])` stays true — one page, two answers, split exactly along which of the two
 * objects a browser would have handed the page. */
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
   and is what NAMED access (`window.myIframeName`) is declared on, so the chain is THREE links and not two:
   window -> Window.prototype -> WindowProperties -> EventTarget.prototype. What stood here said the third link
   "is not built here, so it is not claimed here either" and that "the third arrives with named access" — named
   access DID arrive, window_install builds the object below, and the sentence went on describing a shorter
   chain than the one this file constructs. A comment that says a thing is absent is the stale claim nobody
   greps, because it reads as a completed decision rather than as a question. */

/* HTML §7.2.2.3 Named access on the Window object — and the object it is declared on.
 *
 * THE NUMBER WAS §7.3.3, AT FIVE SITES IN THIS FILE, and §7.3.3 is "Fully active documents". A number that
 * resolves to a REAL section about something else is the citation failure that reads as authoritative: a
 * reader who looks it up finds a heading, not an error, and stops. citegen names the replacement outright, but
 * only once a citation says WHICH STANDARD it belongs to — a bare `§7.3.3` is routed to the file vote and this
 * file votes html either way, so the five sites sat under an auditor that runs over the whole tree. Hence
 * `HTML §7.2.2.3` with the section's own title beside it, here and at each of the four below.
 *
 * `window.myFrameName` and `window.someElementId` are not properties of the global and not properties of
 * Window.prototype: Web IDL puts an interface's NAMED PROPERTIES on a separate object one link further up the
 * chain — window -> Window.prototype -> WindowProperties -> EventTarget.prototype — precisely so that a page's
 * own `window.foo = 1` SHADOWS the named property rather than colliding with it. The corpus walks that chain
 * link by link, so the object is not a formality: without it the chain is short by one and every level below
 * compares against the wrong prototype.
 *
 * THE ORDER IN §7.2.2.3 IS THE WHOLE ALGORITHM, and each branch is a different kind of answer:
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

/* HTML §7.2.2.3 Named access on the Window object's value for `name`, or JS_UNDEFINED when the name is not
   supported. Owned on success. */
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
       and is declared so; and the two indices are all this branch needs, because HTML §7.2.2.3 Named access on
       the Window object only distinguishes none, exactly one, and more than one. */
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
    /* HTML §7.2.2.3 Named access on the Window object: a single named element that HAS a content navigable
       answers with the navigable's WindowProxy —
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

/* WEB IDL §3.7.4.5 [[PreventExtensions]] — the NAMED PROPERTIES OBJECT's, which is a different section from the
   §3.9.5 every legacy platform object obeys and says the same thing: "When the [[PreventExtensions]] internal
   method of a named properties object is called, the following steps are taken: Return false."
   THIS OBJECT IS THAT ONE. §3.7.4 defines it as existing "for every interface declared with the [Global]
   extended attribute that supports named properties", which is Window, and §3.7.4's construction sets exactly
   five overrides — [[GetOwnProperty]], [[DefineOwnProperty]], [[Delete]], [[SetPrototypeOf]] and
   [[PreventExtensions]] — of which this class now carries two. It is not the Window and it is not a legacy
   platform object: it is the link between Window.prototype and EventTarget.prototype window_install builds,
   and a page
   reaches it with one `Object.getPrototypeOf(Window.prototype)`. Without this hook that one expression handed
   a page a freeze of the object every unqualified name in the document resolves through. */
static int win_named_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

static const JSClassExoticMethods WINDOW_PROPS_EXOTIC = {
    .get_own_property = win_named_get_own,
    .prevent_extensions = win_named_prevent_extensions,
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
/* EACH IS `-1` BEFORE ANYTHING RUNS, and the initialiser is the whole of what says so. A pool entry is an
   INDEX — idl_method_id_all hands out `g_n++`, so ZERO IS A VALID ENTRY, the first one the platform declares —
   and every one of these carried no initialiser at all, which made their pre-init value 0 and therefore
   indistinguishable from a real declaration. That is core/agent_state.h's fetch defect exactly (JS_ATOM_NULL is
   a valid atom; entry 0 is a valid member): a second agent's window_install would have installed `close`,
   `stop` and the three setters out of whatever the new pool put at those indices, with a live-looking
   number behind every one and nothing in either of JS_FreeRuntime's censuses to report it. `-1` is what
   idl_install_accessor already reads as "no setter", so it is this file's own sentinel and not a new one. */
static int g_id_close = -1, g_id_stop = -1;   /* declared once per agent — see window_init */
static int g_id_opener_set = -1;   /* §7.2.2.4's `opener` setter, declared with them for the same reason */
static int g_id_name_set = -1;     /* §7.2.2.1's `name` setter — its DOMString conversion is the page's code */
static int g_id_status_set = -1;   /* §7.2.2.5's `status` setter — likewise */

void window_init(JSContext *ctx)
{
    DCHECK(g_window_class == 0, "window_init ran twice — a class is registered once per agent, and a second "
                                "registration would give one interface two ids that compare unequal");
    g_window_rt = JS_GetRuntime(ctx);
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
    /* §7.2.2.5's `status` lives in a PER-REALM record, so the slot it lives in is declared once per AGENT —
       a slot is a class id, and a class id is a registration in the one runtime. */
    DCHECK(g_status_slot < 0, "window_init ran twice — §7.2.2.5's status slot is declared once per agent, and "
                              "a second declaration would leave every realm built under the first one reading "
                              "a slot nothing sets");
    g_status_slot = realm_value_declare(ctx, "HTML §7.2.2.5 the Window's status");
    g_id_opener_set = idl_setter_id(ctx, IDL_ANY, false, js_win_set_opener, 0);
    g_id_name_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_win_set_name, 0);
    g_id_status_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_win_set_status, 0);
    g_id_close = idl_method_id(ctx, NULL, 0, js_win_close, 0);
    /* ONE BODY PER MEMBER. `stop` and §6.6.6's `blur` shared `js_win_noeffect` and were told apart only by a
       magic, which is how §6.6.6's specified do-nothing and §7.2.2.1's unbuilt two steps came to be
       byte-identical at every call site; the magic is unused and stays 0, because what distinguishes them is
       which body runs and not which number it was handed. `blur` is declared by core/html/focus.c now — see
       focus_install_window_members for why §6.6.6's two Window members have to be one list. */
    g_id_stop  = idl_method_id(ctx, NULL, 0, js_win_stop, 0);
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. It declared NOTHING, and its row
       had an EMPTY RELEASE COLUMN, which is the pair of silences that list reads as agreement: a component
       holding everything and giving none of it back is character-for-character the report a component holding
       nothing produces. §7.2.2.5's BarProp declares under this same name, because a sub-component names the row
       whose release reaches it and window_free is what reaches bar_prop_free. */
    agent_state_ptr("window", &g_window_rt,
                    "the runtime §7.2.2's two classes and five member declarations were registered in");
    agent_state_class("window", &g_window_class,
                      "HTML §7.2.2 The Window object's per-realm prototype slot and the brand the global "
                      "carries");
    agent_state_class("window", &g_window_props_class,
                      "HTML §7.2.2.3 Named access on the Window object's WindowProperties per-realm prototype "
                      "slot and brand");
    agent_state_id("window", &g_status_slot,
                   "the per-realm slot HTML §7.2.2.5 Historical browser interface element APIs' `status` "
                   "record lives in");
    agent_state_id("window", &g_id_opener_set,
                   "HTML §7.2.2.4 Accessing related windows' `opener` setter declaration");
    agent_state_id("window", &g_id_name_set,
                   "HTML §7.2.2.1 Opening and closing windows' `name` setter declaration");
    agent_state_id("window", &g_id_status_set,
                   "HTML §7.2.2.5 Historical browser interface element APIs' `status` setter declaration");
    agent_state_id("window", &g_id_close,
                   "HTML §7.2.2.1 Opening and closing windows' `close` declaration");
    agent_state_id("window", &g_id_stop,
                   "HTML §7.2.2.1 Opening and closing windows' `stop` declaration");
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
    /* §7.2.2's IDL marks `top` `[LegacyUnforgeable] readonly attribute WindowProxy? top;` — an OWN property —
       but its VALUE is the navigable's, and `top` is a WALK of the parent chain, so a grandchild answers with
       the top-level traversable rather than with itself. An own ACCESSOR, not an own value frozen at install
       time — which is exactly what idl_install_accessor_unforgeable defines, at the same [[Enumerable]]: true /
       [[Configurable]]: false Web IDL §3.7.6 gives an unforgeable attribute.
       IT WAS A HAND-ROLLED JS_DefinePropertyGetSet, and that is why it is written here now: idl_args.c mints
       every plain-C attribute getter at ONE point so that a getter installed on the realm's global gets
       §3.7.6's opening steps — the receiver resolution, §3.5's "getter" security check and the Window brand —
       without any member having to remember them. A define that goes around that mint is a member that silently
       does not have them, and `top` is on HTML §7.2.1.3.1 CrossOriginProperties with [[NeedsGetter]] true, so
       it is one of the names for which that check has an answer other than "refuse". */
    idl_install_accessor_unforgeable(ctx, g, "top", js_win_top, 0, -1);
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
    /* §7.2.2.5's `status`, and its BASELINE record — "when the Window object is created, the attribute must be
       set to the empty string" is this line, and it runs with the realm rather than on first touch so the
       empty string belongs to the baseline every flow forks from instead of to whichever flow read first. */
    {
        JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(rec), "this realm's §7.2.2.5 status record could not be allocated");
        JS_SetPropertyStr(ctx, rec, "status", JS_NewString(ctx, ""));
        realm_value_set(ctx, g_status_slot, rec);
    }
    idl_install_accessor(ctx, g, "status", js_win_get_status, 0, g_id_status_set);

    /* §7.2.2.4 `frameElement` — the element this navigable is nested THROUGH. */
    idl_install_accessor(ctx, g, "frameElement", js_win_frame_element, 0, -1);

    /* `closed` is a GETTER over the NAVIGABLE's per-flow state, because close() changes it. */
    idl_install_accessor(ctx, g, "closed", js_win_closed, 0, -1);
    idl_install_replaceable(ctx, g, "length", js_win_length, 0);   /* [Replaceable] readonly */
    idl_install_method(ctx, g, "close", g_id_close);
    /* §6.6.6's `Window.focus()` AND `Window.blur()` — installed by the component that owns §6.6.4's steps, as
       ONE list, because §7.2.1.3.1 CrossOriginProperties puts both names on §7.2.3's WindowProxy surface too
       and a two-member list installed from two files drifts with nothing to say so. */
    focus_install_window_members(ctx, g);
    /* Selection API §4.2's `Selection? getSelection()`, installed by the component that owns it for the same
       reason. §4.2 defines it as §4.1's member invoked on `this's Window.document`, so it is not a second
       algorithm and the two cannot answer differently. */
    selection_install_window_members(ctx, g);
    idl_install_method(ctx, g, "stop", g_id_stop);

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

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, and it takes the RUNTIME because that is what an
 * agent is. It took a JSContext until this diff, which is the whole of what kept it off that column and made it
 * a hand-written line in three hosts instead — and the three had it in three DIFFERENT PLACES, which is what a
 * hand-copied list does even when no host is missing the line: main.c and test_forced.c ran it between
 * §7.2.6.5's NavigationHistoryEntry and the cross-agent seam, while wpt_runner.c ran it a whole teardown later,
 * after solver_agent_free and after document_free. Reverse declaration order decides it now and no author has
 * to agree with any other. Nothing here ever needed a JSContext: what this gives back is two class ids, a
 * realm-value slot id and five pool entries, every one of which is a registration in the RUNTIME. */
void window_free(JSRuntime *rt)
{
    /* NOT a null check. This runs from a release column that runs only where platform_agent_init ran, and this
       component's declaration is unconditional on that list (core/agent_state.h). */
    DCHECK(g_window_rt != NULL,
           "§7.2.2's Window was released in an agent that never declared it — window_init is a row on "
           "core/platform.c's declare column, so reaching here without it is a teardown of a browser that was "
           "never brought up");
    DCHECK(g_window_rt == rt,
           "§7.2.2's Window was released against a RUNTIME other than the one it was declared in — its two "
           "classes, its realm-value slot and its five pool entries are registrations in that runtime, and "
           "zeroing them against another leaves every one of them standing in the runtime that issued them");
    g_window_class = 0;
    g_window_props_class = 0;
    /* THE SLOT IS THE AGENT'S AND ITS VALUES WENT WITH THE REALMS — a slot id is a class id in a runtime that
       is going away, so what is given back here is the id, exactly as the two class ids above are. window_init
       asserts it is back at -1, which is the half that makes a forgotten reset crash rather than hand a second
       agent a slot in a runtime that no longer exists. */
    g_status_slot = -1;
    /* AND THE FIVE POOL ENTRIES, which this release kept — the same slots window_proxy_free was keeping one file
       over, and the same consequence: a declaration is a registration in a runtime, so a carried index names an
       entry in a pool the next agent has not built, read by the first window_install that agent runs. Their
       pre-init value is -1 and not 0, because entry 0 is a real member (see the declarations above). */
    g_id_opener_set = g_id_name_set = g_id_status_set = -1;
    g_id_close = g_id_stop = -1;
    /* §7.2.2.5's BarProp, which has no row of its own because this release is what reaches it. */
    bar_prop_free(rt);
    g_window_rt = NULL;
}
