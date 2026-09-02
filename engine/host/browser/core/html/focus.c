/* HTML §6.6 — THE FOCUS MODEL: the focused area of the document, §6.6.4's processing model, and §6.6.6's focus
 * management APIs.
 *
 * WHAT WAS HERE BEFORE. `HTMLElement.focus()` and `blur()` were one body returning undefined, under a comment
 * saying "a headless run has no focus ring, and the spec defines no scriptable result for either beyond moving
 * the focus — which is a state this engine does not model", and `Window.focus()` carried the same claim. Both
 * halves of that are false. The focused area of a document is SCRIPTABLE STATE — `document.activeElement` is
 * defined as a read of it, `document.hasFocus()` walks it, and a page's router, its modal manager and its
 * keyboard-trap all branch on it. And moving the focus FIRES EVENTS at the page's own listeners: §6.6.4's focus
 * update steps fire `change`, then `blur` down the old chain, then `focus` up the new one. A no-effect there is
 * not a missing focus ring, it is a whole family of the page's handlers that never runs — which for this
 * engine's purpose is a whole region of the bundle that is never explored.
 *
 * THE STATE IS ONE FACT, AND IT LIVES IN THIS REALM'S BASELINE RECORD — the shape document.c's readiness and
 * §6.2's visibility state already use, for the two reasons stated there: the record is unreachable from the
 * page, so nothing but this component can write the focused area; and the write is an ordinary property write,
 * so the heap COW captures it and one arm of a fork can focus its dialog without touching its sibling's. A
 * flow that focuses an element and parks resumes with that element still focused.
 *
 * WHAT A FOCUSABLE AREA IS IN THIS ENGINE, AND WHY THE INITIAL ONE IS THE VIEWPORT. §6.6.2's table lists five
 * kinds. Two of them — an `area` element's shape and an element's SCROLLABLE REGIONS — are regions of a LAYOUT
 * this engine does not compute, and one — a user-agent SUBWIDGET (a `video` control, a spin button) — is a
 * rendering this engine does not paint; none of the three is reachable from script, so none is modelled and
 * none is invented. The two that ARE: an ELEMENT meeting the row-1 criteria, and the VIEWPORT of a Document
 * with a non-null browsing context, whose DOM anchor is the Document itself.
 * The standard leaves the INITIAL designation to the user agent, and this one designates the VIEWPORT — which
 * is not a shrug and is not "unknown". It is what makes `document.activeElement` answer `document.body` on a
 * page that has focused nothing (§6.6.6's getter steps 4-5 turn a Document candidate into its body), it is what
 * §6.6.6's `blur()` means by "moves the focus to the viewport", and it is the only designation under which
 * `hasFocus()` is true for a document nobody has clicked in. Designating the BODY ELEMENT instead would answer
 * the same for `activeElement` and would then be wrong everywhere else: the body would appear in the focus
 * chain, so focusing anything would fire `blur` at the body, which no browser does.
 *
 * SYSTEM FOCUS. §6.6.2 makes the currently focused area of a top-level traversable null unless the traversable
 * HAS SYSTEM FOCUS, and `hasFocus()` returns false without it. This user agent PRESENTS every document it holds
 * — it computes every step of update-the-rendering and omits only the paint — so its top-level traversable has
 * system focus, exactly as page_visibility.c's initial visibility state is "visible" for the same reason. The
 * one thing that can MOVE system focus between traversables is `window.focus()` on another top-level
 * traversable, which is another instance; that read crosses the instance boundary and is not built, so it
 * aborts where it is asked rather than answering for a traversable this instance cannot see.
 *
 * FIRING A FOCUS EVENT IS A FocusEvent, AND NOTHING LESS IS THE SAME EVENT. §6.6.4's "fire a focus event"
 * fires "using FocusEvent, with the relatedTarget attribute initialized to r, the view attribute initialized to
 * t's node document's relevant global object, and the composed flag set" — three facts a plain Event cannot
 * carry: `e.relatedTarget` is what a page reads to learn where the focus WENT, `e.view` is the document it
 * moved in, and the composed flag is what decides whether the event escapes a shadow tree at all. The interface
 * is core/events/focus_event.c and the mint below is its one internal caller. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_scrolling.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/events/focus_event.h"
#include "core/frame/navigation.h"
#include "core/frame/window_proxy.h"
#include "core/html/focus.h"
#include "core/html/html_element.h"
#include "core/html/html_form.h"
#include "core/html/html_iframe.h"
#include "core/html/integer_microsyntax.h"
#include "core/html/user_activation.h"

/* §6.6.2's FOCUSABLE AREAS AND THE OBJECTS §6.6.4 PASSES AROUND BESIDE THEM. A focus target is not always a
   focusable area — the focusing steps take an element that is not one, or a navigable — and a focus CHAIN holds
   Documents as well as areas. The kind travels WITH the value because the value alone cannot say: the viewport
   of a Document and the Document itself are the same node, and they are two different entries of the chain
   that fire two different events (a viewport fires none; a Document fires at its Window). */
enum { FA_NONE = 0, FA_ELEMENT, FA_VIEWPORT, FA_DOCUMENT, FA_NAVIGABLE };

static int g_focus_slot = -1;
static int g_id_el_focus = -1, g_id_el_blur = -1, g_id_win_focus = -1, g_id_has_focus = -1;
static int g_id_win_blur = -1;        /* §6.6.6's `Window.blur()`, whose method steps are TO DO NOTHING */
static int g_id_set_tab_index = -1;   /* §6.6.3's `tabIndex` setter — the mixin's, installed beside focus/blur */
static int g_id_viewport = -1, g_id_autofocus = -1;
static int g_ready;

/* ---- §6.6.2's data model: the focused area of the document -------------------------------------------------
 *
 * The record holds ONE field, `el`: the focused area's element wrapper, or JS_NULL for THE VIEWPORT. Those are
 * the only two focusable areas this engine models (see the file header), so the field is total — there is no
 * third state and no "none": a Document with a browsing context always has a focused area, and a Document
 * WITHOUT one has no realm record at all, which is how §6.6.6's getter answers null for
 * `implementation.createHTMLDocument("")`. */

/* THE REALM WHOSE ACTIVE DOCUMENT `doc` IS, or NULL — `document_active_realm_of`, and it is the DOM's answer
   rather than this file's. It stood here as a static because §6.6.2's focused area was its first caller; §6.6.7's
   autofocus candidates is the second, and a second copy of "is this document the realm's active one" is the
   one-fact-two-answers defect. It moved to core/dom/document.c, which owns both halves it reads. */

/* This realm's focused area. `*pkind` is FA_ELEMENT or FA_VIEWPORT; the value is OWNED (the element's wrapper,
   or the Document's for the viewport). */
static JSValue focused_area_of(JSContext *realm, int *pkind)
{
    JSValue rec = realm_value_get(realm, g_focus_slot), el;

    DCHECK(JS_IsObject(rec), "a realm answered for its document's §6.6.2 focused area with no record");
    el = JS_GetPropertyStr(realm, rec, "el");
    JS_FreeValue(realm, rec);
    if (JS_IsObject(el)) {
        *pkind = FA_ELEMENT;
        return el;
    }
    DCHECK(JS_IsNull(el), "a focused-area record held something that is neither an element nor the viewport");
    JS_FreeValue(realm, el);
    *pkind = FA_VIEWPORT;
    return JS_DupValue(realm, document_object(realm));
}

/* §6.6.4 step 4.1.2's "DESIGNATE entry as the focused area of the document". `el` is the element, or JS_NULL for
   the viewport — which is what §6.6.6's `blur()` designates through the unfocusing steps. */
static void focused_area_designate(JSContext *realm, JSValueConst el)
{
    JSValue rec = realm_value_get(realm, g_focus_slot);

    DCHECK(JS_IsObject(rec), "a realm was asked to designate a focused area it has no record for");
    JS_SetPropertyStr(realm, rec, "el", JS_DupValue(realm, el));
    JS_FreeValue(realm, rec);
}

/* ---- the navigable tree, as §6.6 walks it ------------------------------------------------------------------ */

/* THE REALM OF THE NAVIGABLE `proxy` NAMES, or NULL when its active document is in another WASM instance. A
   cross-instance focus read is a suspend the peer answers, and this engine has no peer for it yet, so every
   caller DFAILs at its own step rather than answering for a document it cannot see. */
static JSContext *navigable_realm_local(JSContext *ctx, JSValueConst proxy)
{
    if (!JS_IsObject(proxy) || window_proxy_is_remote(proxy)) return NULL;
    return window_proxy_realm(ctx, proxy);
}

/* This realm's navigable's TOP-LEVEL TRAVERSABLE, as a realm, or NULL when it is another instance's. */
static JSContext *top_traversable_realm(JSContext *ctx)
{
    JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
    JSContext *realm = navigable_realm_local(ctx, top);

    JS_FreeValue(ctx, top);
    return realm;
}

/* §6.6.2's "a top-level traversable HAS SYSTEM FOCUS when it can receive keyboard input channeled from the
   operating system". See the file header for the decision: this user agent presents every document it holds, so
   its own top-level traversable has it. The traversable is resolved rather than assumed, because a document
   whose top is in ANOTHER instance cannot answer for that instance's system focus — `window.focus()` on a
   cross-origin popup is what moves it, and that transfer is a cross-instance operation this engine has not
   built. */
static bool traversable_has_system_focus(JSContext *ctx)
{
    JSContext *top = top_traversable_realm(ctx);

    DCHECK(top != NULL, "HTML §6.6.2's system focus was asked of a top-level traversable in ANOTHER WASM "
                        "instance — the answer lives with that instance's engine and reaching it is a "
                        "cross-instance read (SECURITY.md's closed set), which suspends the asking flow and "
                        "resumes it with the answer. Build that request in core/frame/window_proxy.c beside "
                        "the other cross-origin members, and route `window.focus()`'s system-focus transfer "
                        "through it");
    return top != NULL;
}

/* The CONTENT NAVIGABLE of an element, as a realm — NULL for an element that is not a navigable container, has
   no navigable in this flow, or whose navigable is another instance's. Asked of the component that owns the
   slot (html_iframe.c) rather than through `contentWindow`, because that attribute is an IDL accessor and this
   walk runs from C with no flow base under it. */
static JSContext *content_navigable_realm(JSContext *ctx, JSValueConst el)
{
    JSValue nav;
    JSContext *realm;

    if (!JS_IsObject(el) || !iframe_has_navigable(ctx, el)) return NULL;
    nav = iframe_navigable(ctx, el);
    realm = navigable_realm_local(ctx, nav);
    JS_FreeValue(ctx, nav);
    return realm;
}

/* §7.4's CONTAINER of the navigable whose active document `child` is — the `<iframe>` element the child is
   nested through, as a wrapper (OWNED), and JS_UNDEFINED for a top-level navigable.
 *
 * IT IS COMPUTED FROM THE PARENT'S TREE, because the (element -> navigable) association is the only one this
 * engine keeps: html_iframe.c stores the navigable on the ELEMENT'S wrapper, per flow, which is what makes a
 * frame appended in one arm of a fork invisible in the other. The reverse answer is therefore a walk of the
 * parent document, and it must be: a stored back-pointer would be one answer for every flow, which is the defect
 * class CLAUDE.md names. `node_wrap_peek` rather than `node_wrap` because an element WITH a navigable
 * necessarily has a wrapper (the slot lives on it), so nothing needs minting to ask. */
static JSValue navigable_container_of(JSContext *child)
{
    JSValue proxy_parent = window_proxy_parent_navigable(child, document_window_proxy(child));
    JSValueConst own = document_window_proxy(child);
    JSContext *parent;
    lxb_dom_node_t *root, *n;

    if (!JS_IsObject(proxy_parent)) { JS_FreeValue(child, proxy_parent); return JS_UNDEFINED; }
    parent = navigable_realm_local(child, proxy_parent);
    JS_FreeValue(child, proxy_parent);
    DCHECK(parent != NULL, "HTML §6.6.2's focus chain climbed out of a document whose PARENT navigable is in "
                           "another WASM instance — the chain then spans two instances and its blur/focus "
                           "events fire in both, so the climb is a cross-instance read (SECURITY.md's closed "
                           "set) that suspends the flow. Build it in core/frame/window_proxy.c");
    if (!parent) return JS_UNDEFINED;
    root = document_root_node(parent);
    for (n = root; n; n = node_next_in(n, root)) {
        JSValueConst wrap;
        JSValue nav;
        bool mine;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        wrap = node_wrap_peek(n);
        if (!JS_IsObject(wrap) || !iframe_has_navigable(parent, wrap)) continue;
        nav = iframe_navigable(parent, wrap);
        mine = JS_VALUE_GET_PTR(nav) == JS_VALUE_GET_PTR(own);
        JS_FreeValue(parent, nav);
        if (mine) return JS_DupValue(parent, wrap);
    }
    DFAIL("HTML §6.6.2's focus chain found no navigable container for a document whose navigable HAS a parent "
          "— every child navigable is created by §4.8.5's insertion steps from an element that keeps it, so a "
          "parent holding no such element means the element was removed without its navigable being destroyed");
    return JS_UNDEFINED;
}

/* ---- §6.6.2's "focusable area": is this ELEMENT one -------------------------------------------------------- */

static const lxb_char_t *el_attr(const lxb_dom_node_t *n, const char *name, size_t *plen)
{
    *plen = 0;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    return lxb_dom_element_get_attribute(lxb_dom_interface_element((lxb_dom_node_t *)n),
                                         (const lxb_char_t *)name, strlen(name), plen);
}

static bool el_is(const lxb_dom_node_t *n, const char *local)
{
    size_t qn = 0;
    const lxb_char_t *q;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    q = lxb_dom_element_qualified_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &qn);
    return q && qn == strlen(local) && !memcmp(q, local, qn);
}

/* §6.6.3's TABINDEX VALUE of an element: the attribute run through §2.3.4.1's rules, or null. The rules are
   core/html/integer_microsyntax.c's — they were a private copy here whose accumulation into a `long` was
   signed-integer overflow for `tabindex="99999999999999999999"`, and the value that undefined behaviour
   produced decided the SIGN this file branches on. `*pv` is written only on success; what §6.6.3 needs is
   whether the string IS an integer, which a plain atoi cannot say (atoi("x") is 0, a tabindex that makes an
   element focusable).
   AN OUT-OF-RANGE TABINDEX IS NOT A PARSE FAILURE. §2.3.4.1 returns the number and §6.6.3's null is for "if
   parsing fails", so a run too large for a `long long` is a successful parse of a positive value — which is
   what its sign says, and the sign is the whole of what both callers ask. */
static bool tabindex_value(const lxb_dom_node_t *n, long long *pv)
{
    size_t len = 0;
    const lxb_char_t *v = el_attr(n, "tabindex", &len);
    HtmlInteger parsed;

    if (!v || !html_parse_integer((const char *)v, len, &parsed)) return false;
    *pv = parsed.overflow ? (parsed.negative ? LLONG_MIN : LLONG_MAX) : parsed.value;
    return true;
}

/* §6.3's INERT. "By default, a node is not inert", and the two things that make one inert are the `inert`
   content attribute on the node or a flat-tree ancestor, and §6.3.1's blocking by a MODAL DIALOG. The second
   contributes nothing in this build and that is asserted rather than assumed: a document is blocked only by the
   topmost dialog in its TOP LAYER, the top layer is filled by `showModal`, and rendering.c's step 23 assert
   already names `HTMLDialogElement.prototype.showModal` as the producer that does not exist — so the day it
   does, that assert fires and this walk gains its second clause. */
static bool node_is_inert(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;
    size_t len = 0;

    for (a = n; a; a = a->parent)
        if (a->type == LXB_DOM_NODE_TYPE_ELEMENT && el_attr(a, "inert", &len)) return true;
    return false;
}

/* §6.6.2 row 1's last criterion, "the element is BEING RENDERED, or delegating its rendering to its children,
   or being used as relevant canvas fallback content", is asked of core/dom/element_view.h — see the ONE
   answer there. It used to be a private walk here, and CSSOM VIEW §6's "has an associated box" is the SAME
   predicate under the other of its two names (HTML defines being rendered AS having layout boxes), so a second
   copy would have been one fact with two answers the day either moved. */

/* HTML §4.11.2 The summary element — "A summary element is a summary for its parent details if the following
   algorithm returns true: 1. If this summary element has no parent, then return false. 2. Let parent be this
   summary element's parent. 3. If parent is not a details element, then return false. 4. If parent's first
   summary element child is not this summary element, then return false. 5. Return true."
   ONE ANSWER, because §6.6.3 asks it TWICE and asks it in two different sentences: the focusability list below
   spells it out ("the first summary element child of a details element") and the tabIndex getter's default
   names the definition ("is a summary element that is a summary for its parent details"). Written at each site
   they would be one fact with two answers the day §4.11.2's algorithm changes under either. */
static bool el_is_summary_for_parent_details(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *p, *c;

    if (!el_is(n, "summary")) return false;
    p = n->parent;                        /* steps 1-2 */
    if (!el_is(p, "details")) return false;                       /* step 3 */
    for (c = p->first_child; c; c = c->next)                      /* step 4 */
        if (el_is(c, "summary")) return c == n;
    return false;
}

/* §6.6.3's "MODULO PLATFORM CONVENTIONS, it is SUGGESTED that the following elements should be considered as
   focusable areas" — the standard's own list, which is what "determined by the user agent to be focusable"
   means for a user agent with no platform conventions of its own to differ by. Every entry is a fact about the
   element's markup, so every entry is decidable here.
   IT IS NOT THE SAME LIST AS THE `tabIndex` GETTER'S DEFAULT, which is why the two are two functions. This one
   admits an `a` only WITH an `href`, an `input` only when its type is not Hidden, an editing host and any
   element carrying `draggable`; that one admits `a`, `area`, `frame`, `iframe`, `input` and `object`
   unconditionally and admits none of the last three. A shared predicate would have to be wrong for one of
   them. */
static bool el_is_ua_focusable(const lxb_dom_node_t *n)
{
    size_t len = 0;

    if (el_is(n, "a") || el_is(n, "area")) return el_attr(n, "href", &len) != NULL;
    if (el_is(n, "button") || el_is(n, "select") || el_is(n, "textarea")) return true;
    if (el_is(n, "input")) {
        const lxb_char_t *t = el_attr(n, "type", &len);
        return !(t && len == 6 && !strncasecmp((const char *)t, "hidden", 6));
    }
    /* "summary elements that are the FIRST summary element child of a details element" — §4.11.2's algorithm,
       above, which is the definition that sentence restates. */
    if (el_is(n, "summary")) return el_is_summary_for_parent_details(n);
    if (el_attr(n, "draggable", &len)) return true;
    /* An EDITING HOST: an element whose `contenteditable` is in the true or plaintext-only state. */
    {
        const lxb_char_t *ce = el_attr(n, "contenteditable", &len);
        if (ce && !(len == 5 && !strncasecmp((const char *)ce, "false", 5))) return true;
    }
    return el_is(n, "iframe");   /* a navigable container */
}

/* §6.6.2's TABLE ROW 1, in full. */
static bool el_is_focusable_area(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);
    lxb_dom_node_t *sr;
    long long ti;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (!tabindex_value(n, &ti) && !el_is_ua_focusable(n)) return false;
    /* "the element is either not a shadow host, or has a shadow root whose DELEGATES FOCUS is false" — a host
       that delegates is not itself a focusable area, which is what sends §6.6.4 to the focus delegate. */
    sr = shadow_root_of_element(ctx, lxb_dom_interface_element(n));
    if (sr && shadow_root_flag(ctx, sr, SHADOW_ROOT_DELEGATES_FOCUS)) return false;
    if (html_form_control_is_disabled(ctx, el)) return false;   /* "not actually disabled" */
    if (node_is_inert(n)) return false;
    return element_view_has_box(n);
}

/* §6.6.2's SEQUENTIALLY FOCUSABLE, as this user agent determines it: a focusable area is in its Document's
   sequential focus navigation order unless its tabindex value is a NEGATIVE INTEGER, which §6.6.3 states as
   "the user agent must consider the element as a focusable area, but should omit the element from any
   tabindex-ordered focus navigation scope". A user agent may narrow it further for its user's sake (macOS skips
   non-form controls); this one has no user to have a preference. */
static bool el_is_sequentially_focusable(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);
    long long ti;

    if (!el_is_focusable_area(ctx, el)) return false;
    return !(tabindex_value(n, &ti) && ti < 0);
}

/* ---- §6.6.4's "get the focusable area" and its two delegates -----------------------------------------------
 *
 * THE STANDARD STATES THIS RECURSIVELY AND SAYS SO ("this step can end up recursing"), and the recursion's
 * depth is the PAGE's: one level per NESTED DELEGATING SHADOW HOST. A C self-call would therefore put a stack
 * frame per host on the C stack, which is the one thing this engine's trampoline exists to make impossible —
 * navigable.c's tree walk is iterative for exactly this reason. So the recursion is an EXPLICIT STACK of walk
 * frames here, one per level the standard would have recursed into, and the C stack stays flat however deeply
 * a component library nests its hosts. */

/* ONE LEVEL of §6.6.4's focus delegate search: the subtree being walked, the cursor into it, which of the two
   passes is running, and whether THIS level's focus target is a `dialog` (which narrows the second pass to
   SEQUENTIALLY focusable areas). A level's target is the host it was entered through, so `is_dialog` is that
   host's own answer and not the outermost target's. */
typedef struct {
    lxb_dom_node_t *root, *cur;
    uint8_t         pass;       /* 0 = the autofocus delegate's pass, 1 = the plain descendant pass */
    uint8_t         is_dialog;
} DelegateFrame;

/* §6.6.4's GET THE FOCUSABLE AREA, minus the ONE branch that recurses. `*pdescend` is the shadow root the
   delegate search must walk when the answer is "this is a delegating shadow host and nothing here decided it";
   NULL on every other branch. Splitting it out is what makes the driver below a loop rather than a self-call —
   the recursive edge becomes a PUSH. */
static void focusable_area_here(JSContext *ctx, int kind, JSValueConst v, int *pk, JSValue *pv,
                                lxb_dom_node_t **pdescend)
{
    lxb_dom_node_t *n;

    *pk = FA_NONE;
    *pv = JS_UNDEFINED;
    *pdescend = NULL;
    if (kind == FA_NAVIGABLE) {
        /* "If focus target is a navigable: return the navigable's ACTIVE DOCUMENT." A Document is not itself a
           focusable area, and the focusing steps go on to build a chain from it — which is what makes
           `window.focus()` a document-level move rather than an element one. */
        JSContext *realm = navigable_realm_local(ctx, v);

        DCHECK(realm != NULL, "HTML §6.6.4's get the focusable area was given a navigable whose active document "
                              "is in another WASM instance — focusing it is that instance's operation, reached "
                              "by a cross-instance request (SECURITY.md's closed set). Build it in "
                              "core/frame/window_proxy.c");
        if (!realm) return;
        *pk = FA_DOCUMENT;
        *pv = JS_DupValue(realm, document_object(realm));
        return;
    }
    DCHECK(kind == FA_ELEMENT, "HTML §6.6.4's get the focusable area was given something that is neither an "
                               "element nor a navigable — the algorithm is stated over exactly those two");
    n = node_of(v);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    /* THE TWO ROWS THIS ENGINE DOES NOT MODEL come first in the standard's list and are absent here for the
       reason the file header gives: an `area` element's SHAPES and an element's SCROLLABLE REGIONS are regions
       of a layout this engine does not compute, so no focus target ever matches them. */
    /* "If focus target is the DOCUMENT ELEMENT of its Document: return the Document's VIEWPORT." */
    if (n->owner_document && n == document_document_element_of(lxb_dom_interface_node(n->owner_document))) {
        JSContext *realm = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
        if (realm) {
            *pk = FA_VIEWPORT;
            *pv = JS_DupValue(realm, document_object(realm));
        }
        /* A document element whose Document has no browsing context: §6.6.2's viewport row requires one, so
           there is no viewport and no focusable area — the standard's own "Otherwise: return null". */
        return;
    }
    /* "If focus target is a NAVIGABLE CONTAINER with a non-null content navigable: return the content
       navigable's active document." */
    {
        JSContext *inner = content_navigable_realm(ctx, v);
        if (inner) {
            *pk = FA_DOCUMENT;
            *pv = JS_DupValue(inner, document_object(inner));
            return;
        }
    }
    /* "If focus target is a SHADOW HOST whose shadow root's DELEGATES FOCUS is true." */
    {
        lxb_dom_node_t *sr = shadow_root_of_element(ctx, lxb_dom_interface_element(n));
        JSContext *top;

        if (!sr || !shadow_root_flag(ctx, sr, SHADOW_ROOT_DELEGATES_FOCUS)) return;
        /* Its step 1: a host that is a shadow-including inclusive ancestor of the currently focused area's DOM
           anchor KEEPS that area — which is what stops re-focusing a delegating host from pulling the focus
           back to its first control. */
        top = top_traversable_realm(ctx);
        if (top) {
            int fk;
            JSValue focused = focused_area_of(top, &fk);
            lxb_dom_node_t *anchor = node_of(focused);

            if (fk == FA_ELEMENT && anchor && shadow_root_is_shadow_including_inclusive_ancestor(n, anchor)) {
                *pk = FA_ELEMENT;
                *pv = focused;
                return;
            }
            JS_FreeValue(top, focused);
        }
        *pdescend = sr;   /* the FOCUS DELEGATE's own step 3: look inside the shadow root */
    }
}

/* §6.6.4's FOCUS DELEGATE and AUTOFOCUS DELEGATE, as the ONE iterative search they are together.
 *
 * Per level: the AUTOFOCUS pass first (the first descendant, in tree order, carrying an `autofocus` content
 * attribute that is or yields a focusable area), then the PLAIN pass (the first descendant that is or yields
 * one at all — sequentially focusable only, when this level's target is a `dialog`). A descendant that yields
 * by DELEGATION pushes a level instead of recursing; a level that runs out pops back to the one that entered
 * it, at the cursor it had. That is the standard's recursion, exactly, on the heap.
 *
 * The FOCUS TRIGGER is "other" at every call site this build has — the "click" trigger belongs to §6.6.2's
 * "when a user ACTIVATES a click focusable focusable area", and this user agent has no user to activate one —
 * so the autofocus delegate's click-focusable filter rejects nothing and is not written as a condition that
 * can never be false.
 *
 * The walk at each level is over DESCENDANTS and not shadow-including descendants, which the standard's own
 * note insists on: a nested host is reached through the delegating branch above, and that is what makes the
 * delegation STOP at a boundary that does not delegate. */
static void delegate_search(JSContext *ctx, lxb_dom_node_t *root, bool is_dialog, int *pk, JSValue *pv)
{
    DelegateFrame *stack;
    size_t depth = 1, cap = 8;

    *pk = FA_NONE;
    *pv = JS_UNDEFINED;
    stack = js_malloc(ctx, cap * sizeof *stack);
    CHECK(stack != NULL, "focus: OOM building §6.6.4's focus delegate search stack");
    stack[0].root = root;
    stack[0].cur = root;
    stack[0].pass = 0;
    stack[0].is_dialog = is_dialog;
    while (depth) {
        DelegateFrame *f = &stack[depth - 1];
        lxb_dom_node_t *descend = NULL;
        JSValue wrap;
        size_t len = 0;
        bool hit;

        f->cur = node_next_in(f->cur, f->root);
        if (!f->cur) {
            if (f->pass == 0) { f->pass = 1; f->cur = f->root; continue; }   /* the autofocus pass is over */
            depth--;                                                          /* this level found nothing */
            continue;
        }
        if (f->cur->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (f->pass == 0 && !el_attr(f->cur, "autofocus", &len)) continue;
        wrap = node_wrap(ctx, f->cur);
        /* The autofocus pass takes any focusable area; the plain pass narrows to SEQUENTIALLY focusable ones
           when this level's target is a dialog, which is what stops a `<dialog>` handing its focus to a
           `tabindex="-1"` wrapper. */
        hit = (f->pass == 0 || !f->is_dialog) ? el_is_focusable_area(ctx, wrap)
                                              : el_is_sequentially_focusable(ctx, wrap);
        if (hit) {
            *pk = FA_ELEMENT;
            *pv = wrap;
            js_free(ctx, stack);
            return;
        }
        focusable_area_here(ctx, FA_ELEMENT, wrap, pk, pv, &descend);
        if (*pk != FA_NONE) {
            JS_FreeValue(ctx, wrap);
            js_free(ctx, stack);
            return;
        }
        if (!descend) { JS_FreeValue(ctx, wrap); continue; }
        {
            bool dialog_level = el_is(f->cur, "dialog");

            JS_FreeValue(ctx, wrap);
            if (depth == cap) {
                DelegateFrame *grown = js_realloc(ctx, stack, cap * 2 * sizeof *stack);
                CHECK(grown != NULL, "focus: OOM growing §6.6.4's focus delegate search stack — the depth is "
                                     "the page's nesting of delegating shadow hosts");
                stack = grown;
                cap *= 2;
            }
            stack[depth].root = descend;
            stack[depth].cur = descend;
            stack[depth].pass = 0;
            stack[depth].is_dialog = dialog_level;
            depth++;
        }
    }
    js_free(ctx, stack);
}

/* §6.6.4's GET THE FOCUSABLE AREA, whole: the branches above plus the delegate search the recursive one enters.
   `*pk` is FA_NONE with `*pv` undefined for the standard's "Otherwise: return null". */
static void get_focusable_area(JSContext *ctx, int kind, JSValueConst v, int *pk, JSValue *pv)
{
    lxb_dom_node_t *descend = NULL;

    focusable_area_here(ctx, kind, v, pk, pv, &descend);
    if (*pk == FA_NONE && descend)
        delegate_search(ctx, descend, el_is(node_of(v), "dialog"), pk, pv);
}

/* ---- §6.6.2's focus chain ---------------------------------------------------------------------------------
 *
 * A chain is an Array of ALTERNATING (kind, value) pairs — the kind as a number, the value as a node wrapper.
 * It is a JS Array rather than a malloc'd list because it is state a flow HOLDS across a suspension: its
 * mutations are property writes the heap COW captures, and the snapshot machinery carries it to the cold tier
 * and back, which a C list of node pointers could do neither of. */

static void chain_push(JSContext *ctx, JSValueConst chain, uint32_t *n, int kind, JSValueConst v)
{
    JS_SetPropertyUint32(ctx, (JSValue)chain, *n * 2, JS_NewInt32(ctx, kind));
    JS_SetPropertyUint32(ctx, (JSValue)chain, *n * 2 + 1, JS_DupValue(ctx, v));
    (*n)++;
}

static int chain_kind(JSContext *ctx, JSValueConst chain, uint32_t i)
{
    JSValue k = JS_GetPropertyUint32(ctx, chain, i * 2);
    int32_t v = 0;

    JS_ToInt32(ctx, &v, k);
    JS_FreeValue(ctx, k);
    DCHECK(v > FA_NONE && v <= FA_NAVIGABLE, "a focus chain entry carried a kind §6.6 does not define");
    return (int)v;
}

/* OWNED. */
static JSValue chain_value(JSContext *ctx, JSValueConst chain, uint32_t i)
{
    return JS_GetPropertyUint32(ctx, chain, i * 2 + 1);
}

/* §6.6.2's FOCUS CHAIN of `subject`, into the Array `chain`; `*pn` is the number of entries.
 *
 * ONE DEVIATION FROM THE STANDARD'S CURRENT WORDING, AND IT IS THE STANDARD THAT IS WRONG. Step 3.2's second
 * clause reads "if currentObject is a Document whose node navigable's PARENT is non-null, then set
 * currentObject to currentObject's node navigable's parent" — a NAVIGABLE, which is neither a focusable area
 * nor a Document, so the very next iteration appends it and breaks. The chain would then stop at the child
 * document, and §6.6.6's own note about `activeElement` ("if the user moves the focus to a text control in an
 * iframe, the IFRAME is the element returned by the activeElement API in the iframe's node document") could
 * never be true, because nothing would ever designate the iframe as the parent document's focused area. The
 * step's own note says the chain "continues up the focus hierarchy up to the Document of the TOP-LEVEL
 * traversable", which the navigable reading also contradicts. The pre-navigable wording set currentObject to
 * the browsing context CONTAINER, and that is what is implemented: the container element, which is a focusable
 * area (a navigable container is one), so the walk continues into the parent Document. */
static void focus_chain_build(JSContext *ctx, JSValueConst chain, uint32_t *pn, int kind, JSValueConst subject)
{
    JSValue cur = JS_DupValue(ctx, subject);
    int k = kind;

    *pn = 0;
    for (;;) {
        lxb_dom_node_t *n = node_of(cur);
        JSContext *realm;

        chain_push(ctx, chain, pn, k, cur);                                             /* step 3.1 */
        /* Step 3.2's second clause — "if currentObject's DOM anchor is an element that is not currentObject
           itself" — reaches only the area shapes, scrollable regions and subwidgets this engine does not model
           (see the file header), so nothing is ever appended by it: an element focusable area IS its own DOM
           anchor, and a viewport's is a Document rather than an element. */
        if (k == FA_ELEMENT || k == FA_VIEWPORT) {                                      /* step 3.3, first arm */
            lxb_dom_node_t *doc = k == FA_VIEWPORT ? n
                                                   : (n && n->owner_document
                                                          ? lxb_dom_interface_node(n->owner_document) : NULL);
            realm = document_active_realm_of(doc);
            if (!realm) break;
            JS_FreeValue(ctx, cur);
            cur = JS_DupValue(realm, document_object(realm));
            k = FA_DOCUMENT;
            continue;
        }
        DCHECK(k == FA_DOCUMENT, "a focus chain walked into an entry that is neither an area nor a Document");
        realm = document_active_realm_of(n);
        if (!realm) break;
        {
            JSValue container = navigable_container_of(realm);

            if (!JS_IsObject(container)) { JS_FreeValue(ctx, container); break; }   /* step 3.3's "break" */
            JS_FreeValue(ctx, cur);
            cur = container;
            k = FA_ELEMENT;
        }
    }
    JS_FreeValue(ctx, cur);
}

/* §6.6.2's CURRENTLY FOCUSED AREA OF A TOP-LEVEL TRAVERSABLE. `*pk` is FA_NONE when the traversable does not
   have system focus. The value is OWNED. */
static void currently_focused_area(JSContext *ctx, int *pk, JSValue *pv)
{
    JSContext *realm;

    *pk = FA_NONE;
    *pv = JS_UNDEFINED;
    if (!traversable_has_system_focus(ctx)) return;                                     /* step 1 */
    realm = top_traversable_realm(ctx);                                                 /* step 2 */
    if (!realm) return;
    for (;;) {                                                                          /* step 3 */
        JSContext *inner;
        int k;
        JSValue area = focused_area_of(realm, &k);

        inner = k == FA_ELEMENT ? content_navigable_realm(realm, area) : NULL;
        if (!inner) {
            *pk = k;                                                                    /* steps 4-5 */
            *pv = area;
            return;
        }
        JS_FreeValue(realm, area);
        realm = inner;
    }
}

/* ---- §6.6.4's "fire a focus event" ------------------------------------------------------------------------- */

/* "To fire a focus event named e at an element t with a given related target r, fire an event named e at t,
   USING FocusEvent, with the relatedTarget attribute initialized to r, the view attribute initialized to t's
   NODE DOCUMENT'S RELEVANT GLOBAL OBJECT, and the COMPOSED FLAG SET." This is the EVENT; the fire is the
   caller's, because §2.9's dispatch is a request this machine parks on and the same object is fired once and
   released.
   IT IS THE CHAIN ENTRY THAT NAMES THE DOCUMENT, NOT THE EVENT TARGET. Steps 2.2 and 4.2 make the event target
   of a DOCUMENT entry that document's relevant global object, so `t` is then a Window — and asking a Window for
   its node document is a question about the wrong object. The entry is the Element or the Document itself,
   which is what §6.6.4 means by "t's node document" in both cases.
   AND THE EVENT IS MINTED IN THAT DOCUMENT'S REALM. A focus move into a same-origin iframe fires at that
   document's listeners, and DOM §2.5 creates an event in the relevant realm of its target: minted here instead
   it would chain to the running document's FocusEvent.prototype and `view` would be a Window belonging to
   neither. The two facts are one — the realm the event is built in is the realm whose global `view` is.
   SO IT TAKES NO RUNNING REALM, which is the point rather than an omission: every value it needs is a fact
   about the ENTRY, and there is nothing left here for the realm that happens to be executing to decide. */
static JSValue fire_a_focus_event_new(int kind, JSValueConst entry, const char *type,
                                      bool bubbles, JSValueConst related)
{
    lxb_dom_node_t *n = node_of(entry);
    JSContext *realm;
    JSValueConst view;

    DCHECK(kind == FA_ELEMENT || kind == FA_DOCUMENT,
           "§6.6.4's fire a focus event was reached for a chain entry that fires none — a viewport's event "
           "target is null and the caller returns before the mint");
    DCHECK(n != NULL, "§6.6.4's fire a focus event was given a chain entry that is not a node — every entry "
                      "that has an event target is an Element or a Document");
    realm = document_realm_of(n);
    DCHECK(realm != NULL, "§6.6.4's fire a focus event reached a node whose document has no realm — a focus "
                          "chain is built from the currently focused area of a top-level traversable, so every "
                          "document in it is some realm's active document");
    view = document_window_of(n);   /* t's node document's relevant global object */
    DCHECK(JS_IsObject(view), "§6.6.4's fire a focus event reached a document with no relevant global object — "
                              "a document with no browsing context has no focused area and cannot be in a "
                              "focus chain at all");
    return focus_event_new_to_fire(realm, type, bubbles, related, view);
}

/* ---- §6.6.6's activeElement and hasFocus ------------------------------------------------------------------- */

/* §6.6.6's DocumentOrShadowRoot `activeElement` getter steps, over a receiver that is a Document or a
   ShadowRoot — ONE implementation, because the two differ only in what candidate is retargeted AGAINST, which
   is step 2's own operand. */
static JSValue js_active_element(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *doc, *cand_node;
    JSContext *realm;
    JSValue candidate, retargeted;
    int kind;

    (void)magic;
    if (!n || (n->type != LXB_DOM_NODE_TYPE_DOCUMENT && !shadow_root_is(n)))
        return JS_ThrowTypeError(ctx, "this is not a Document or a ShadowRoot");
    /* Step 1: "let candidate be this's NODE DOCUMENT's focused area's DOM anchor". A shadow root's node
       document is the document of the tree it is in, which is where the focused area lives. */
    doc = n->type == LXB_DOM_NODE_TYPE_DOCUMENT
              ? n : (n->owner_document ? lxb_dom_interface_node(n->owner_document) : NULL);
    realm = document_active_realm_of(doc);
    if (!realm) return JS_NULL;   /* a Document with no focused area — one createHTMLDocument built */
    candidate = focused_area_of(realm, &kind);
    DCHECK(kind == FA_ELEMENT || kind == FA_VIEWPORT,
           "§6.6.6's activeElement read a focused area that is neither an element nor a viewport");
    /* The DOM ANCHOR: an element focusable area is its own, and a viewport's is its Document — which is why
       the getter's steps 4-6 exist at all. */
    retargeted = event_target_retarget(ctx, candidate, this_val);                       /* step 2 */
    JS_FreeValue(ctx, candidate);
    cand_node = node_of(retargeted);
    if (!cand_node || node_root(cand_node) != n) {                                      /* step 3 */
        JS_FreeValue(ctx, retargeted);
        return JS_NULL;
    }
    if (cand_node->type != LXB_DOM_NODE_TYPE_DOCUMENT) return retargeted;               /* step 4 */
    JS_FreeValue(ctx, retargeted);
    {
        lxb_dom_node_t *body = document_body_of(cand_node);                             /* step 5 */
        if (body) return node_wrap(ctx, body);
    }
    return node_wrap(ctx, document_document_element_of(cand_node));                      /* steps 6-7 */
}

/* §6.6.6's HAS FOCUS STEPS, given a Document target. */
static JSValue js_has_focus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSContext *target, *realm;

    (void)argc; (void)argv; (void)magic;
    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "this is not a Document");
    target = document_active_realm_of(n);
    /* A Document with no browsing context has no node navigable and therefore no top-level traversable, so
       step 1's condition cannot be met: `implementation.createHTMLDocument("").hasFocus()` is false. */
    if (!target) return JS_FALSE;
    if (!traversable_has_system_focus(target)) return JS_FALSE;                         /* step 1 */
    realm = top_traversable_realm(target);                                              /* step 2 */
    if (!realm) return JS_FALSE;
    for (;;) {                                                                          /* step 3 */
        JSContext *inner;
        int k;
        JSValue area;

        if (realm == target) return JS_TRUE;                                            /* step 3.1 */
        area = focused_area_of(realm, &k);
        inner = k == FA_ELEMENT ? content_navigable_realm(realm, area) : NULL;          /* step 3.2 */
        JS_FreeValue(realm, area);
        if (!inner) return JS_FALSE;                                                    /* step 3.3 */
        realm = inner;
    }
}

/* ---- §6.6.4's three algorithms, as ONE machine ------------------------------------------------------------- */

/* THEY ARE ONE MACHINE BECAUSE THEY ARE ONE ALGORITHM WITH THREE ENTRY POINTS: the unfocusing steps end by
   running the focusing steps or the focus update steps, and the focusing steps end by running the focus update
   steps. Written as three machines they would be three states handing each other a suspension, which is the
   seam CLAUDE.md's §C-stack rule is about; written as one, the entry point is the member's magic and every
   stage below is a step of the standard.
 *
 * IT RESTS AT EVERY EVENT, AND THAT IS THE POINT. Each fire is the PAGE'S code — a handler that loops, awaits
 * or mutates the tree — so the machine parks there and a sibling flow overtakes it, and it resumes with its
 * chains and its cursors intact. It also rests once per chain entry, so a chain of nested documents cannot be
 * walked to completion inside one opcode. */
#define FOCUS_STAGES(X) \
    X(FOC_ENTER,    "HTML §6.6.6 focus()/blur() step 1 (the algorithm's focus target, and the Document the " \
                    "allow focus steps are given)") \
    X(FOC_ALLOW,    "HTML §6.6.6 focus() step 1 → the ALLOW FOCUS STEPS' second clause (target's relevant " \
                    "global object has TRANSIENT ACTIVATION — unknown external state, so the flow forks here " \
                    "and the refused arm and the focusing arm are two worlds)") \
    X(FOC_UNFOCUS,  "HTML §6.6.4 unfocusing steps steps 1-7 (the delegating host, inert, the old chain, and " \
                    "the branch on the top document's system focus)") \
    X(FOC_AREA,     "HTML §6.6.4 focusing steps steps 1-5 (get the focusable area over the focus delegate " \
                    "search, the fallback target, the navigable container, inert, and the already-focused " \
                    "early return)") \
    X(FOC_CHAINS,   "HTML §6.6.4 focusing steps steps 6-7 and focus update steps step 1 (the old chain, the " \
                    "new chain, and popping their common tail)") \
    X(FOC_CHANGE,   "HTML §6.6.4 focus update steps step 2.1 (one old-chain entry: fire `change` at an input " \
                    "the user edited without committing)") \
    X(FOC_BLUR,     "HTML §6.6.4 focus update steps steps 2.2-2.4 (the blur event target, the related blur " \
                    "target, and fire a focus event named `blur`)") \
    X(FOC_FOCUSOUT, "UI Events §3.3.2's focus event order (`focusout` follows `blur`, bubbling, at the same " \
                    "target with the same related target)") \
    X(FOC_FOCUS,    "HTML §6.6.4 focus update steps steps 4.1-4.4 (one new-chain entry in reverse order: " \
                    "designate the focused area of the document, then fire a focus event named `focus`)") \
    X(FOC_FOCUSIN,  "UI Events §3.3.2's focus event order (`focusin` follows `focus`, bubbling, at the same " \
                    "target with the same related target)")
enum { IDL_STEP_STAGE_BASE(FOCUS_STAGES) FOCUS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FOCUS_STEPS[] = { FOCUS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The entry points, as the magic each declaration carries. FOCUS_VIEWPORT and FOCUS_AUTOFOCUS are not §6.6.6
   members and are on no prototype: one is HTML §8.1.7.3 step 17's "run the focusing steps for doc's viewport"
   and the other HTML §6.6.7 flush autofocus candidates step 5.11.3's "run the focusing steps for target", each
   reached through an internal door at the foot of this file. They are ENTRY POINTS rather than second machines
   for the reason the other three are — the focusing steps they need are these, and a copy of them would be a
   copy of the chains, the pops and the four events. */
enum { FOCUS_EL_FOCUS = 0, FOCUS_EL_BLUR, FOCUS_WIN_FOCUS, FOCUS_VIEWPORT, FOCUS_AUTOFOCUS };

typedef struct {
    uint8_t  fphase;                 /* the fire request's own phase */
    uint8_t  ua_phase;               /* the allow focus steps' activation question's own phase */
    uint8_t  kind;                   /* the current focus target's kind */
    JSValue  target;                 /* the current focus target (owned) */
    /* THE DOCUMENT THE ALLOW FOCUS STEPS ARE GIVEN, as its WindowProxy (owned; JS_UNDEFINED where the entry
       point has no step 1 to run). It is CARRIED rather than re-derived at the stage that asks, because that
       stage is a rest point: the question it asks is over unknown state, so the flow can park there and
       another flow can run, and an operation that becomes a work item takes its inputs with it. A raw
       JSContext * would say the same thing and say it in a form no snapshot can carry across a session. */
    JSValue  allow_win;
    JSValue  old_chain, new_chain;   /* Arrays of alternating (kind, value) (owned) */
    uint32_t old_n, new_n;           /* how many ENTRIES of each are live — step 1's pop shortens both */
    uint32_t i, j;                   /* step 2's cursor, and step 4's count of entries already visited */
    JSValue  ev;                     /* the event being fired (owned) */
    /* THE FIRE REQUEST'S BUFFER, AS THE TYPE THAT CARRIES ITS WIDTH. It was `JSValue cb[4]` for a dispatch
       that takes [this, dispatch, target, event, targetOverride] — five slots — so the FIRST focus event this
       machine ever managed to mint would have dupped one past the end of the array, over whatever field
       follows it. event_target.h names the type for exactly that reason: a caller that writes the number
       cannot be anything but an argument behind the algorithm. */
    EventFireCb cb;
} FocusState;

static void focus_state_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FocusState *s = st;
    int k;

    v->val(ctx, &s->target);
    v->val(ctx, &s->allow_win);
    v->val(ctx, &s->old_chain);
    v->val(ctx, &s->new_chain);
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

/* Every owned field in place before the first thing that can fail — the failure path tears this state down
   through focus_state_visit, which names exactly what the state owns and nothing else. */
static bool focus_state_init(JSContext *ctx, FocusState *s)
{
    int k;

    s->target = s->ev = s->allow_win = JS_UNDEFINED;
    s->old_chain = s->new_chain = JS_UNDEFINED;
    s->kind = FA_NONE;
    s->old_n = s->new_n = s->i = s->j = 0;
    s->fphase = 0;
    s->ua_phase = 0;
    STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
    s->old_chain = JS_NewArray(ctx);
    s->new_chain = JS_NewArray(ctx);
    return !JS_IsException(s->old_chain) && !JS_IsException(s->new_chain);
}

/* §6.6.6's ALLOW FOCUS STEPS, given a Document target.
     "If target is allowed to use the `focus-without-user-activation` feature, return true."  HTML §4.8.5 "The
   `iframe` element" defines that phrase and core/permissions_policy/permissions_policy.h answers it, over the
   Document's own Permissions Policy §9.5 policy — HTML §2.2 "Policy-controlled features" gives the feature a
   default allowlist of 'self'.
     THIS FILE USED TO COMPUTE THE ANSWER ITSELF as "same origin with the top-level traversable's active
   document", and that sentence appears in no step of §9.7: inheritance is a CHAIN through every container, so a
   document nested A(top) → B(cross-origin) → C(same origin as A) is same origin with the top and is DISABLED.
   It was also the second copy of one question, which is why it is now a call.
     "If target's relevant global object has TRANSIENT ACTIVATION, return true."  Asked of §6.4.1's real state
   (core/html/user_activation.c), never assumed — and asked THROUGH THE FORK SEAM, which is why this is a
   request and not a `bool` function. Whether a user has interacted with the page is unknown external state
   (user_activation.h says why it is a source and not a constant), so the second clause has two feasible
   answers and both reach code worth running: `el.focus()` refused, and `el.focus()` running the whole of
   §6.6.4 with its four events. A C `if` here would pick one of those and delete the other, which is the one
   thing a solver exists to prevent.
     THE FIRST CLAUSE STILL SHORT-CIRCUITS, and it does the work: a document whose whole container chain is
   same-origin is allowed to use the feature outright, so nothing is asked and nothing forks — which is every
   document in an ordinary page. The question is reached only where the inherited policy does not already
   answer it.
     IT IS NOT EXPORTED. It was, so that §6.6.7's insertion steps could answer this clause from inside DOM
   §4.2.3's plain-C walk and crash on the other; that walk now carries the driving machine's header and forks
   like anything else, so the whole algorithm has one door and this clause has no caller of its own. */
static bool focus_allow_without_user_activation(JSContext *target)
{
    return document_allowed_to_use(target, PP_FEATURE_FOCUS_WITHOUT_USER_ACTIVATION);
}

int focus_allow_focus_steps_run(JSContext *target, JSStepHdr *h, uint8_t *phase, bool *out)
{
    if (focus_allow_without_user_activation(target)) {
        *out = true;
        return 0;
    }
    /* THE SECOND CLAUSE IS A FORK, so from here on there must be a machine to fork AT — and the one caller
       that has none is §6.6.7's walk over the tree the PARSER built, which runs inside document install, at
       the pre-boot COW baseline, where no flow exists to snapshot and nothing would receive the JS_STEP_FORK.
       Asserted at the question rather than at that caller, because the caller cannot know whether the
       question will be reached without re-deciding the clause above — and a second copy of that decision is
       exactly the one-fact-two-answers defect this file exists to avoid.
       BUILD: §6.6.7's insertion steps for the parsed tree belong to the FIRST FLOW rather than to the
       baseline install — give document install's parsed-tree walks (§4.8.5's child navigables, §13.2.6.4.4's
       declarative shadow roots and autofocus_document_parsed) a step machine that boot drives, and hand its
       header down here. Only a CROSS-ORIGIN-embedded document's parsed tree reaches this line: every document
       that is same origin with its top-level document is answered by the clause above. */
    DCHECK(h != NULL,
           "§6.6.6's ALLOW FOCUS STEPS reached their second clause — §6.4.1's TRANSIENT ACTIVATION, which is "
           "unknown external state and therefore a FORK — with no driving step machine to fork at. That is "
           "§6.6.7's walk over the tree the parser built, which runs inside document install at the pre-boot "
           "COW baseline: make those parsed-tree walks a step machine the first flow drives, and pass its "
           "header here");
    return user_activation_transient_run(target, h, phase, out);
}

/* The event target for a chain entry — §6.6.4 steps 2.2 and 4.2, which are the same three cases twice: an
   element is itself, a Document is its relevant global object, and anything else fires nothing. BORROWED. */
static JSValueConst chain_event_target(int kind, JSValueConst v)
{
    if (kind == FA_ELEMENT) return v;
    if (kind == FA_DOCUMENT) return document_window_of(node_of(v));
    return JS_NULL;   /* a viewport fires nothing, which is the standard's own note */
}

/* §6.6.4 steps 2.3 / 4.3's RELATED TARGET: the last entry of the OTHER chain, but only when both ends are
   Elements. OWNED (JS_NULL when there is none). */
static JSValue chain_related(JSContext *ctx, JSValueConst chain, uint32_t n, bool at_last)
{
    if (!at_last || n == 0 || chain_kind(ctx, chain, n - 1) != FA_ELEMENT) return JS_NULL;
    return chain_value(ctx, chain, n - 1);
}

static int focus_step(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FocusState *s = state;
    int magic = idl_step_magic(hdr);
    int r;

    *presult = JS_UNDEFINED;
    /* THE DELIVERED VALUE BELONGS TO THE ONE REQUEST THIS MACHINE MAKES. Only a fire is outstanding when this
       is re-entered with a value, and `fphase` is exactly the flag for that — so the value is released here,
       once, on every other re-entry, and no stage below has to remember whether it is the one that consumes. */
    if (s->fphase == 0) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
    }
    for (;;) {
        switch (hdr->stage) {
        case FOC_ENTER: {
            lxb_dom_node_t *n;
            JSContext *doc;

            if (!focus_state_init(ctx, s)) return -1;
            if (magic == FOCUS_WIN_FOCUS) {
                /* §6.6.6's Window focus() steps: the navigable, the allow focus steps over its ACTIVE
                   DOCUMENT, then the focusing steps with the navigable itself. There is no notification to
                   trigger — step 5's is a user-agent presentation with no scriptable result.
                   THE DOCUMENT THE ALLOW FOCUS STEPS ARE GIVEN IS CARRIED to the stage that asks, as its
                   WindowProxy: that stage is a rest point, so the operand travels with the work item instead
                   of being read back off the running realm after other flows have run.
                 *
                 * STEP 1 IS "LET CURRENT BE *THIS*'S NAVIGABLE", AND IT IS THE RECEIVER'S — NOT THE RUNNING
                 * REALM'S. This read `document_window_proxy(ctx)`, which is Web IDL §3.7.7 Operations' `this`
                 * only for the ONE receiver that happens to be the member's own realm's global. It was correct
                 * by accident and by accident only: `otherW.focus()` reached the OTHER realm's own function
                 * object, whose `ctx` js_call_c_function takes from the callee, so the realm answered what the
                 * receiver should have. The moment §7.2.1.3.1's list put this member on §7.2.3's WindowProxy
                 * surface — where §7.2.1.3.4 CrossOriginGetOwnPropertyHelper mints it "in the CURRENT realm"
                 * and performs its steps "on object O" — that accident inverts: one function object, minted in
                 * the READING realm, would have focused the reader's own navigable for every window a page
                 * called it on. window_proxy.h states the rule at the helper; this is the site it applies to. */
                JSValueConst nav;

                nav = window_proxy_this_navigable(ctx, hdr->this_val);
                if (JS_IsUninitialized(nav)) return -1;   /* §3.7.7's TypeError, already thrown */
                /* STEP 2: "If current is null, then return." */
                if (window_proxy_navigable_null(ctx, nav)) return 0;
                /* THE PEER'S NAVIGABLE IS A SUSPEND, NOT A REALM, and it is asserted HERE rather than left to
                   the helpers below because those are shared: window_proxy_realm's crash names the seam for
                   every caller in the engine, and get-the-focusable-area's names window_proxy.c without saying
                   which operation wanted it. §6.6.6's is the one that has to travel — see the sentence in
                   traversable_has_system_focus, which is the SAME transfer asked one step later. */
                DCHECK(!window_proxy_is_remote(nav),
                       "HTML §6.6.6 Focus management APIs' `Window.focus()` was called on a navigable whose "
                       "active document is in ANOTHER WASM instance — its step 3 runs the allow focus steps "
                       "over THAT document and its step 4 the focusing steps in THAT agent, so neither can be "
                       "answered out of this heap. Build it as a cross-instance request beside §7.2.1.3.1's "
                       "other members in core/frame/window_proxy.c: the flow SUSPENDS here, the peer runs "
                       "§6.6.6's steps in its own scheduled turn under this flow's world, and the flow resumes "
                       "with the completion. It is the same transfer traversable_has_system_focus names for "
                       "§6.6.2's system focus, and the two land together");
                s->kind = FA_NAVIGABLE;
                s->target = JS_DupValue(ctx, nav);
                s->allow_win = JS_DupValue(ctx, nav);
                STEP_GOTO(hdr->stage, FOC_ALLOW, &s->fphase, &s->ua_phase, NULL);
                continue;
            }
            if (magic == FOCUS_VIEWPORT) {
                /* HTML §8.1.7.3 step 17. The step names the ALGORITHM and its new focus target and nothing
                   else: there are no allow focus steps here (those are §6.6.6's members' own step 1, and this
                   is not a member — the user agent is repairing its own designation, not honouring a page's
                   request) and no options.
                   THE VIEWPORT IS THIS REALM'S, not a receiver's. The door mints this function in the realm
                   whose Document is being fixed up, so the machine's own ctx IS doc — which is what makes a
                   child navigable's `blur` fire at the child's Window (§3.7) with no operand to get wrong. */
                DCHECK(node_of(document_object(ctx)) != NULL,
                       "§8.1.7.3 step 17's focus fixup ran in a realm with no Document, so there is no viewport "
                       "for §6.6.4's focusing steps to be given");
                s->kind = FA_VIEWPORT;
                s->target = JS_DupValue(ctx, document_object(ctx));
                STEP_GOTO(hdr->stage, FOC_AREA, &s->fphase, &s->ua_phase, NULL);
                continue;
            }
            if (magic == FOCUS_AUTOFOCUS) {
                /* HTML §6.6.7's flush autofocus candidates, step 5.11.3: "Run the focusing steps for target."
                   THE TARGET IS THE CANDIDATE ELEMENT ITSELF, and step 1 below re-derives the focusable area
                   from it — which is the same derivation §6.6.7 step 5.10 already made to decide that target
                   was non-null, and necessarily the same answer, because §6.6.7's steps 5.11.1 and 5.11.2 are
                   two writes to its own list and its own flag and run not one line of the page's code.
                   THERE ARE NO ALLOW FOCUS STEPS HERE. §6.6.7 runs them at INSERTION (its own step 5), on the
                   document the candidate was inserted into and at the moment it was; by the time the rendering
                   algorithm flushes, the decision has been taken and this is the user agent carrying it out. */
                DCHECK(argc == 1,
                       "§6.6.7 step 5.11.3's door was entered with something other than its one focus target");
                n = node_of(argv[0]);
                DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
                       "§6.6.7 step 5.11.3 was given a focus target that is not an element — an autofocus "
                       "candidate is an element carrying the `autofocus` content attribute, and nothing else "
                       "reaches this door");
                DCHECK(n != NULL && n->owner_document != NULL &&
                           document_active_realm_of(lxb_dom_interface_node(n->owner_document)) == ctx,
                       "§6.6.7 step 5.11.3's door was minted in a realm that is not the candidate's own node "
                       "document — the focus chain, the events' globals and the viewport are all read off this "
                       "machine's ctx, so a candidate in a CHILD document must be focused through the CHILD's "
                       "door (§6.6.7 step 5.4 lets a candidate belong to any document under topDocument's "
                       "top-level traversable, so this is the ordinary case and not the exotic one)");
                s->kind = FA_ELEMENT;
                s->target = JS_DupValue(ctx, argv[0]);
                STEP_GOTO(hdr->stage, FOC_AREA, &s->fphase, &s->ua_phase, NULL);
                continue;
            }
            /* WEB IDL §3.7.7 Operations' brand check — a TypeError, not an assert, because
               `HTMLElement.prototype.focus.call(null)` is a thing the corpus does deliberately. */
            n = node_of(hdr->this_val);
            if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                JS_ThrowTypeError(ctx, "this is not an element");
                return -1;
            }
            s->kind = FA_ELEMENT;
            s->target = JS_DupValue(ctx, hdr->this_val);
            if (magic == FOCUS_EL_BLUR) {
                STEP_GOTO(hdr->stage, FOC_UNFOCUS, &s->fphase, &s->ua_phase, NULL);
                continue;
            }
            DCHECK(magic == FOCUS_EL_FOCUS,
                   "the focus machine was declared with a magic that is none of its entry points — every "
                   "algorithm that enters §6.6.4 declares one here, and a new one arrives with the branch that "
                   "sets up its focus target");
            /* §6.6.6's focus(options) step 1. Steps 3 and 4 are decided in this arm, where the OPTIONS are, and
               both are answered rather than deferred:
                 step 3's "indicate focus" is the focus ring — a user-agent presentation with no scriptable
                   result, which is the one shape §NO STUBS permits as a documented no-effect;
                 step 4's "scroll a target into view" needs a scrolling box that can be at a position other than
                   the one this engine derives, and no box in this build can be — asserted below, the way
                   rendering.c asserts its own unwritten steps, so the day CSSOM VIEW §3.1 "Scrolling"'s perform
                   a scroll arrives this fires and names the step to write.
               STEP 4's ASSERT ASKED FOR THE NAME `scrollTo` ON THE GLOBAL, with its own [[GetOwnProperty]]
                   rather than `realm_awaits`, and the name was never the capability. `Element.prototype
                   .scrollTo` is installed in this build and moves nothing; installing CSSOM VIEW §4's
                   `scrollTo` on the Window would have satisfied the test and moved nothing either, so the
                   DCHECK would have fired announcing a scrolling box that could still only be at its origin —
                   a probe reporting a capability as PRESENT, which is worse than no probe. The question now
                   goes to the component that owns §3.1 (core/dom/element_scrolling.h), and it is asked of THIS'S
                   NODE DOCUMENT rather than of the running realm, for the same reason step 1's allow focus
                   steps are: §6.1's scroll a target into view walks out of the element to ITS document's
                   viewport. That is why it sits below the `doc` resolution rather than above it. */
            /* Step 1's allow focus steps are given THIS'S NODE DOCUMENT, not the running realm's — an element
               adopted into another same-origin document is focused under that document's policy. A document
               with no browsing context has neither the feature nor an activation, so its elements are not
               focusable through this member at all. */
            doc = n->owner_document ? document_active_realm_of(lxb_dom_interface_node(n->owner_document)) : NULL;
            if (!doc) return 0;
            DCHECK(!element_scrolling_box_can_move(doc) ||
                   idl_dict_bool(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "preventScroll"),
                   "HTML §6.6.6 \"Focus management APIs\" focus() step 4 — \"if options[\"preventScroll\"] is "
                   "false, then scroll a target into view given this, \"auto\", \"center\", and \"center\"\" — "
                   "must be written here: a scrolling box in this element's document can now be at a position "
                   "other than the one this engine derives. The algorithm step 4 calls is already exported "
                   "(core/dom/element_scrolling.h's element_scrolling_scroll_target_into_view), so what is "
                   "missing is a stage of this machine AFTER §6.6.4's focusing steps, with `scrollIntoView`'s "
                   "own step 7 — the target has an associated box — asked first");
            s->allow_win = JS_DupValue(ctx, document_window_proxy(doc));
            STEP_GOTO(hdr->stage, FOC_ALLOW, &s->fphase, &s->ua_phase, NULL);
            continue;
        }

        case FOC_ALLOW: {
            /* §6.6.6 step 1's ALLOW FOCUS STEPS, over the Document FOC_ENTER handed this stage. The second of
               their two clauses is §6.4.1's transient activation, which is unknown external state, so this is
               where the flow forks: one arm is `focus()` refused and the page's fallback, the other runs the
               whole of §6.6.4 and its four events. A stage of its own because a machine may have exactly one
               request outstanding, and re-entering a stage that asks twice would re-ask the first forever. */
            bool allowed = false;
            JSContext *doc;
            int r;

            DCHECK(window_proxy_is(s->allow_win),
                   "§6.6.6 step 1 reached the allow focus steps with no Document to run them over — every "
                   "entry point that sets this stage sets the WindowProxy beside it");
            doc = window_proxy_realm(ctx, s->allow_win);
            r = focus_allow_focus_steps_run(doc, hdr, &s->ua_phase, &allowed);
            if (r) return r;
            if (!allowed) return 0;
            STEP_GOTO(hdr->stage, FOC_AREA, &s->fphase, &s->ua_phase, NULL);
            continue;
        }

        case FOC_UNFOCUS: {
            /* §6.6.4's UNFOCUSING STEPS, given the element `blur()` was called on. */
            int fk;
            JSValue focused;
            JSContext *top = top_traversable_realm(ctx);
            lxb_dom_node_t *n;
            uint32_t e;
            bool in_chain = false;

            if (!top) return 0;
            currently_focused_area(ctx, &fk, &focused);
            /* Step 1: a DELEGATING HOST that contains the currently focused area unfocuses THAT area. */
            n = node_of(s->target);
            if (fk == FA_ELEMENT && n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_node_t *sr = shadow_root_of_element(ctx, lxb_dom_interface_element(n));
                lxb_dom_node_t *anchor = node_of(focused);
                if (sr && shadow_root_flag(ctx, sr, SHADOW_ROOT_DELEGATES_FOCUS) && anchor &&
                    shadow_root_is_shadow_including_inclusive_ancestor(sr, anchor)) {
                    JS_FreeValue(ctx, s->target);
                    s->target = JS_DupValue(ctx, focused);
                    s->kind = FA_ELEMENT;
                    n = anchor;
                }
            }
            if (n && node_is_inert(n)) { JS_FreeValue(ctx, focused); return 0; }         /* step 2 */
            /* Step 3's area-shape and scrollable-region substitutions reach kinds this engine does not model
               (see the file header), so the old focus target is unchanged by them.
               Step 4: "let old chain be the CURRENT FOCUS CHAIN of the top-level traversable in which old focus
               target finds itself" — the current focus chain is the chain of the CURRENTLY FOCUSED AREA, not of
               the old focus target. Building it from the target instead would make step 5's membership test
               trivially true, and `blur()` on an element that never had the focus would run the whole update. */
            if (fk == FA_NONE) { JS_FreeValue(ctx, focused); return 0; }
            focus_chain_build(ctx, s->old_chain, &s->old_n, fk, focused);               /* step 4 */
            JS_FreeValue(ctx, focused);
            for (e = 0; e < s->old_n; e++) {                                            /* step 5 */
                JSValue v = chain_value(ctx, s->old_chain, e);
                bool same = chain_kind(ctx, s->old_chain, e) == s->kind &&
                            JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(s->target);
                JS_FreeValue(ctx, v);
                if (same) { in_chain = true; break; }
            }
            if (!in_chain) return 0;
            if (!el_is_focusable_area(ctx, s->target)) return 0;                        /* step 6 */
            /* Step 7: topDocument is the old chain's LAST entry, and its navigable HAS system focus (this
               engine's own traversable does, and the chain ends in it), so the branch taken is the first —
               run the FOCUSING STEPS for topDocument's viewport. That is what §6.6.6 means by "blur() moves
               the focus to the viewport". */
            {
                JSContext *doc_realm;
                JSValue last = chain_value(ctx, s->old_chain, s->old_n - 1);

                DCHECK(chain_kind(ctx, s->old_chain, s->old_n - 1) == FA_DOCUMENT,
                       "§6.6.4's unfocusing steps step 7 reached an old chain whose last entry is not a "
                       "Document — the chain is built up to the top-level traversable's Document by "
                       "construction");
                doc_realm = document_active_realm_of(node_of(last));
                DCHECK(doc_realm != NULL, "§6.6.4's topDocument is not the active document of any realm");
                DCHECK(traversable_has_system_focus(ctx),
                       "§6.6.4's unfocusing steps step 7 reached its SECOND branch: topDocument's navigable "
                       "does not have system focus, so the platform-specific conventions for REMOVING system "
                       "focus run and the focus update steps are given an EMPTY new chain. Nothing in this "
                       "build can clear system focus, so reaching it means one was added without this branch");
                JS_FreeValue(ctx, s->target);
                s->target = last;
                s->kind = FA_VIEWPORT;
                s->old_n = 0;   /* the focusing steps build their own chains from the current state */
                STEP_GOTO(hdr->stage, FOC_AREA, &s->fphase, &s->ua_phase, NULL);
                continue;
            }
        }

        case FOC_AREA: {
            /* §6.6.4's FOCUSING STEPS, steps 1-5. */
            int fk;
            JSValue focused;

            if (s->kind == FA_NAVIGABLE ||
                (s->kind == FA_ELEMENT && !el_is_focusable_area(ctx, s->target))) {     /* step 1 */
                int k;
                JSValue v;

                get_focusable_area(ctx, s->kind, s->target, &k, &v);
                JS_FreeValue(ctx, s->target);
                s->target = v;
                s->kind = (uint8_t)k;
                /* Step 2: no fallback target is passed by either member, so a null area ends the algorithm. */
                if (k == FA_NONE) return 0;
            }
            /* Step 3: a navigable container that is a focusable area still focuses its content navigable's
               active document, which is what makes `iframe.focus()` focus the frame's document. */
            if (s->kind == FA_ELEMENT) {
                JSContext *inner = content_navigable_realm(ctx, s->target);
                if (inner) {
                    JS_FreeValue(ctx, s->target);
                    s->target = JS_DupValue(inner, document_object(inner));
                    s->kind = FA_DOCUMENT;
                }
            }
            if ((s->kind == FA_ELEMENT || s->kind == FA_VIEWPORT) && node_is_inert(node_of(s->target)))
                return 0;                                                               /* step 4 */
            currently_focused_area(ctx, &fk, &focused);                                 /* step 5 */
            {
                bool already = fk == (int)s->kind && JS_IsObject(focused) &&
                               JS_VALUE_GET_PTR(focused) == JS_VALUE_GET_PTR(s->target);
                JS_FreeValue(ctx, focused);
                if (already) return 0;
            }
            STEP_GOTO(hdr->stage, FOC_CHAINS, &s->fphase, &s->ua_phase, NULL);
            continue;
        }

        case FOC_CHAINS: {
            /* Steps 6-7, and the focus update steps' own step 1. */
            int fk;
            JSValue focused;

            currently_focused_area(ctx, &fk, &focused);                                 /* step 6 */
            s->old_n = 0;
            if (fk != FA_NONE) focus_chain_build(ctx, s->old_chain, &s->old_n, fk, focused);
            JS_FreeValue(ctx, focused);
            focus_chain_build(ctx, s->new_chain, &s->new_n, s->kind, s->target);        /* step 7 */
            /* FOCUS UPDATE STEPS STEP 1: "if the last entry in old chain and the last entry in new chain are
               the same, POP the last entry from each and redo this step" — which is what leaves a focus move
               inside one document firing nothing at that document or at anything above it. */
            while (s->old_n && s->new_n) {
                JSValue a = chain_value(ctx, s->old_chain, s->old_n - 1);
                JSValue b = chain_value(ctx, s->new_chain, s->new_n - 1);
                bool same = chain_kind(ctx, s->old_chain, s->old_n - 1) ==
                                chain_kind(ctx, s->new_chain, s->new_n - 1) &&
                            JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);

                JS_FreeValue(ctx, a);
                JS_FreeValue(ctx, b);
                if (!same) break;
                s->old_n--;
                s->new_n--;
            }
            s->i = 0;
            s->j = 0;
            STEP_GOTO(hdr->stage, FOC_CHANGE, &s->fphase, &s->ua_phase, NULL);
            continue;
        }

        case FOC_CHANGE: {
            /* Step 2.1. Its condition is "the USER has changed the element's value or its list of selected
               files while the control was focused WITHOUT COMMITTING that change" — a user edit, which this
               user agent has no input device to make, and which a script's own `input.value = x` explicitly is
               not (no browser fires `change` for one either). So no old-chain entry meets it and no `change`
               is fired; the stage still exists because the day an input device is modelled, this is where its
               uncommitted edit is committed. */
            if (s->i >= s->old_n) { STEP_GOTO(hdr->stage, FOC_FOCUS, &s->fphase, &s->ua_phase, NULL); continue; }
            STEP_GOTO(hdr->stage, FOC_BLUR, &s->fphase, &s->ua_phase, NULL);
            continue;
        }

        case FOC_BLUR:
        case FOC_FOCUSOUT: {
            /* Steps 2.2-2.4, then UI Events' bubbling twin. One old-chain entry per pass. */
            bool blur = hdr->stage == FOC_BLUR;
            JSValueConst tgt;
            JSValue entry = chain_value(ctx, s->old_chain, s->i);
            int kind = chain_kind(ctx, s->old_chain, s->i);

            tgt = chain_event_target(kind, entry);
            if (!JS_IsObject(tgt)) {
                JS_FreeValue(ctx, entry);
                if (blur) { STEP_GOTO(hdr->stage, FOC_FOCUSOUT, &s->fphase, &s->ua_phase, NULL); continue; }
                s->i++;
                STEP_GOTO(hdr->stage, FOC_CHANGE, &s->fphase, &s->ua_phase, NULL);
                return JS_STEP_YIELD;   /* one chain entry per step */
            }
            if (JS_IsUndefined(s->ev)) {
                JSValue related = chain_related(ctx, s->new_chain, s->new_n,
                                                kind == FA_ELEMENT && s->i + 1 == s->old_n);
                s->ev = fire_a_focus_event_new(kind, entry, blur ? "blur" : "focusout", !blur, related);
                JS_FreeValue(ctx, related);
                if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, entry); return -1; }
            }
            r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), tgt, s->ev, JS_UNDEFINED, cb_result,
                                      NULL, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, entry);
            if (r > 0) return r;
            if (r < 0) return -1;
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            s->fphase = 0;
            if (blur) { STEP_GOTO(hdr->stage, FOC_FOCUSOUT, &s->fphase, &s->ua_phase, NULL); continue; }
            s->i++;
            STEP_GOTO(hdr->stage, FOC_CHANGE, &s->fphase, &s->ua_phase, NULL);
            continue;
        }

        case FOC_FOCUS:
        case FOC_FOCUSIN: {
            /* Steps 4.1-4.4 over the new chain IN REVERSE ORDER, then UI Events' bubbling twin. */
            bool focus = hdr->stage == FOC_FOCUS;
            uint32_t at;
            JSValueConst tgt;
            JSValue entry;
            int kind;

            if (s->j >= s->new_n) return 0;   /* the focus update steps are finished */
            at = s->new_n - 1 - s->j;
            entry = chain_value(ctx, s->new_chain, at);
            kind = chain_kind(ctx, s->new_chain, at);
            /* Step 4.1 runs ONCE per entry, before the event: the guard is that no fire has begun for this
               entry yet. Without it a resume from inside the `focus` handler would re-enter this stage and
               designate a second time — undoing a handler that focused something else, which is a state change
               out of an algorithm that had already made it. */
            if (focus && s->fphase == 0 && JS_IsUndefined(s->ev) &&
                (kind == FA_ELEMENT || kind == FA_VIEWPORT)) {                          /* step 4.1 */
                lxb_dom_node_t *n = node_of(entry);
                lxb_dom_node_t *doc = kind == FA_VIEWPORT
                                          ? n : (n && n->owner_document
                                                     ? lxb_dom_interface_node(n->owner_document) : NULL);
                JSContext *realm = document_active_realm_of(doc);

                DCHECK(realm != NULL, "§6.6.4 step 4.1.2 designates the focused area of a document that is no "
                                      "realm's active document");
                if (realm) {
                    /* Step 4.1.1: "set the document's relevant global object's NAVIGATION API's `focus changed
                       during ongoing navigation` to true" — HTML §7.2.6.8's flag, which §8.1.7.3's update-the-
                       rendering step 17 clears when it repairs a focused area and which §7.2.6.10.5's
                       potentially-reset-the-focus reads to decide whether an intercepted navigation may move
                       the focus. It is set in the realm whose document is being designated, not in this one. */
                    navigation_set_focus_changed(realm, true);
                    focused_area_designate(realm, kind == FA_VIEWPORT ? JS_NULL : entry);
                }
            }
            tgt = chain_event_target(kind, entry);
            if (!JS_IsObject(tgt)) {
                JS_FreeValue(ctx, entry);
                if (focus) { STEP_GOTO(hdr->stage, FOC_FOCUSIN, &s->fphase, &s->ua_phase, NULL); continue; }
                s->j++;
                return JS_STEP_YIELD;   /* one chain entry per step */
            }
            if (JS_IsUndefined(s->ev)) {
                JSValue related = chain_related(ctx, s->old_chain, s->old_n,
                                                kind == FA_ELEMENT && at + 1 == s->new_n);
                s->ev = fire_a_focus_event_new(kind, entry, focus ? "focus" : "focusin", !focus, related);
                JS_FreeValue(ctx, related);
                if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, entry); return -1; }
            }
            r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), tgt, s->ev, JS_UNDEFINED, cb_result,
                                      NULL, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, entry);
            if (r > 0) return r;
            if (r < 0) return -1;
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            s->fphase = 0;
            if (focus) { STEP_GOTO(hdr->stage, FOC_FOCUSIN, &s->fphase, &s->ua_phase, NULL); continue; }
            s->j++;
            continue;
        }

        default:
            DFAIL("the focus machine resumed into a stage HTML §6.6.4 does not have");
            return 0;
        }
    }
}

static const IdlStepDecl EL_FOCUS_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, NULL,
    "HTML §6.6.6 HTMLOrSVGOrMathMLElement.focus(options), over §6.6.4's focusing and focus update steps",
    FOCUS_STEPS
};
static const IdlStepDecl EL_BLUR_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, NULL,
    "HTML §6.6.6 HTMLOrSVGOrMathMLElement.blur(), over §6.6.4's unfocusing steps",
    FOCUS_STEPS
};
static const IdlStepDecl WIN_FOCUS_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, NULL,
    "HTML §6.6.6 Window.focus(), over §6.6.4's focusing and focus update steps",
    FOCUS_STEPS
};
static const IdlStepDecl VIEWPORT_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, NULL,
    "HTML §8.1.7.3 update the rendering step 17, over §6.6.4's focusing and focus update steps",
    FOCUS_STEPS
};
static const IdlStepDecl AUTOFOCUS_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, NULL,
    "HTML §6.6.7 flush autofocus candidates step 5.11.3, over §6.6.4's focusing and focus update steps",
    FOCUS_STEPS
};

/* ---- THE INTERNAL DOORS INTO §6.6.4 ------------------------------------------------------------------------
 *
 * MINTED IN THE DOCUMENT'S OWN REALM — event_target.c's dispatcher, for its reason: a step function carries its
 * DEFINING realm, and this machine reads the viewport, the focus chain and the events' globals off that ctx, so
 * the one a child document is reached through has to be the child's. It costs one function object per
 * invocation, which is a cold path (a focused area only stops being focusable when the page changed the tree
 * under it; a document's autofocus candidates are flushed once) and buys the absence of a runtime-lifetime
 * object that would have to be per-realm and freed. It is minted through idl_step_function like every other
 * declared member, which is what keeps the pool's name for it — a hand-written JS_NewCFunction2 leaves the
 * member anonymous in every diagnostic.
 * OWNED by the caller. */
static JSValue focus_door_new(JSContext *ctx, const char *name, int stepid)
{
    JSValue fn;

    DCHECK(stepid >= 0,
           "a C caller reached §6.6.4's focusing steps before focus_init declared them — an internal door is "
           "the only way one reaches them, and there is one focus machine");
    fn = idl_step_function(ctx, name, stepid);
    CHECK(!JS_IsException(fn), "focus: an internal door into §6.6.4's focusing steps could not be allocated");
    return fn;
}

/* ---- HTML §8.1.7.3 step 17's focus fixup rule -------------------------------------------------------------- */

/* The step's CONDITION — "if the focused area of doc is not a focusable area". See focus.h for why it is asked
   here rather than restated at the step. */
bool focus_focused_area_is_focusable(JSContext *ctx)
{
    JSValue area;
    int kind;
    bool ok;

    DCHECK(g_ready, "§8.1.7.3 step 17 asked its condition before focus_init declared §6.6's members");
    area = focused_area_of(ctx, &kind);
    DCHECK(kind == FA_ELEMENT || kind == FA_VIEWPORT,
           "§8.1.7.3 step 17 read a focused area that is neither an element nor a viewport — those are the two "
           "§6.6.2 designations this engine models, and the record holds nothing else");
    /* §6.6.2's table makes the VIEWPORT of a Document with a non-null browsing context a focusable area, and a
       realm holding a §6.6.2 record IS such a Document — so the viewport answers true and this step's whole
       subject is the ELEMENT the tree stopped rendering, disabled, or disconnected under it. */
    ok = kind != FA_ELEMENT || el_is_focusable_area(ctx, area);
    JS_FreeValue(ctx, area);
    return ok;
}

/* The step's ACTION — §6.6.4's focusing steps given doc's viewport, as a request the calling machine parks on.
   Same two-leg shape as event_target_fire_run, and for the same reason: the algorithm runs the page's `blur`,
   `focusout`, `focus` and `focusin` listeners, so it cannot be a call from C. */
int focus_viewport_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValue in,
                       JSValue **out_cb, int *out_argc)
{
    JSValue out = JS_UNDEFINED;
    int r;

    /* ASKED ON BOTH LEGS, because the resume leg forwards the same capacity and a caller that got the first
       one right by accident must not get the second one wrong in silence. */
    DCHECK(cb_cap >= FOCUS_VIEWPORT_CB_SLOTS,
           "§8.1.7.3 step 17's focusing-steps request was handed a buffer narrower than step_call_run's "
           "[this, func] shape");
    if (*phase == 0) {
        JSValue fn = focus_door_new(ctx, "focusingStepsForViewport", g_id_viewport);

        /* step_call_run DUPS the callee into the request buffer, which is what holds it across the suspension —
           so this realm's door is released here and the parked call still owns one. */
        r = step_call_run(ctx, phase, cb, cb_cap, fn, JS_UNDEFINED, 0, NULL, in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        DCHECK(r == JS_STEP_CALL, "§8.1.7.3 step 17's focusing-steps request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 0, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "§6.6.4's focusing steps resumed into something other than their answer");
    DCHECK(JS_IsUndefined(out),
           "§6.6.4's focusing steps answered with a value — the algorithm has no result, so a value here is a "
           "member's return leaking through the door");
    JS_FreeValue(ctx, out);
    return 0;
}

/* ---- HTML §6.6.7's three calls into §6.6 ------------------------------------------------------------------- */

/* §6.6.7 flush step 4's first disjunct, negated: "topDocument's focused area is topDocument ITSELF". §6.6.2's
   focused area of a Document in this engine is either an ELEMENT or the VIEWPORT, and the viewport's DOM anchor
   IS the Document — which is exactly the state the standard's sentence describes and the state a page that has
   focused nothing is in (the file header states why the initial designation is the viewport rather than null).
   So the disjunct is "the focused area is not the viewport", and it is what stops an autofocus from stealing a
   focus the page or a `#fragment` already placed somewhere else. */
bool focus_focused_area_is_viewport(JSContext *ctx)
{
    JSValue area;
    int kind;

    DCHECK(g_ready, "§6.6.7 asked for a focused area before focus_init declared §6.6's members");
    area = focused_area_of(ctx, &kind);
    JS_FreeValue(ctx, area);
    DCHECK(kind == FA_ELEMENT || kind == FA_VIEWPORT,
           "§6.6.7 read a focused area that is neither an element nor a viewport — those are the two §6.6.2 "
           "designations this engine models, and the record holds nothing else");
    return kind == FA_VIEWPORT;
}

/* §6.6.2 Data model's FOCUSED AREA OF THE DOCUMENT, AS ITS DOM ANCHOR — the read HTML §6.12 The popover
   attribute's show popover step 17 and its hide a popover step 20.2 both make, and the only §6.6 read either of
   them makes. §6.6.2 defines the anchor once for every kind of focusable area — "Each focusable area has a DOM
   anchor, which is a Node object that represents the position of the focusable area in the DOM. (When the
   focusable area is itself a Node, it is its own DOM anchor.)" — so an ELEMENT area answers with itself and the
   VIEWPORT with its Document, which is what focused_area_of already returns and what §6.6.6's `activeElement`
   already reads it for. That is why this is a door onto the ONE record and not a second derivation of the
   anchor: a caller that asked `focus_focused_area_is_viewport` and then reached for the element itself would be
   holding the second copy of a rule this file owns.
   THE ANSWER IS A NODE AND NOT AN ELEMENT, which its callers must handle rather than assume away: the viewport
   is the initial designation in this engine (see the file header), so a document nobody has focused in answers
   with the DOCUMENT. OWNED. */
JSValue focus_focused_area_dom_anchor(JSContext *ctx)
{
    JSValue area;
    int kind;

    DCHECK(g_ready, "a §6.6.2 focused area was asked for before focus_init declared §6.6's members");
    area = focused_area_of(ctx, &kind);
    DCHECK(kind == FA_ELEMENT || kind == FA_VIEWPORT,
           "a §6.6.2 focused area was read as something that is neither an element nor a viewport — those are "
           "the two designations this engine models, and the record holds nothing else");
    return area;
}

/* §6.6.7 flush steps 5.9-5.10, as the verdict they exist to reach: "let target be element; if target is not a
   focusable area, then set target to the result of getting the focusable area for target" — and step 5.11 then
   branches on whether that target is null. Both halves are §6.6.4's, so both are answered here.
   IT RUNS NONE OF THE PAGE'S CODE, which is why it is a question and not a request: §6.6.4's delegate search is
   a tree walk over content attributes and shadow roots, with no accessor and no callback in it. */
bool focus_focusable_area_exists(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);
    JSValue v;
    int k;

    DCHECK(g_ready, "§6.6.7 asked for a focusable area before focus_init declared §6.6's members");
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§6.6.7 step 5.9 was given a candidate that is not an element — the autofocus candidates list holds "
           "elements the insertion steps appended and nothing else");
    if (el_is_focusable_area(ctx, el)) return true;                                     /* step 5.10's test */
    get_focusable_area(ctx, FA_ELEMENT, el, &k, &v);
    JS_FreeValue(ctx, v);
    return k != FA_NONE;
}

/* §6.6.7 flush step 5.11.3's ACTION — the focusing steps given the candidate, as a request the calling machine
   parks on. Same two-leg shape and same reason as focus_viewport_run above.
   THE ARGUMENT COUNT IS PASSED ON BOTH LEGS AND IT IS LOAD-BEARING: step_call_run places [this, func, args…]
   on the first leg and releases `2 + argc` slots on the resume, so a resume that forwarded 0 would leak the
   candidate's reference and, with it, the whole tree it is in. */
int focus_element_run(JSContext *ctx, JSValueConst el, uint8_t *phase, JSValue *cb, int cb_cap, JSValue in,
                      JSValue **out_cb, int *out_argc)
{
    JSValue out = JS_UNDEFINED;
    int r;

    /* ASKED ON BOTH LEGS, because the resume leg forwards the same capacity and a caller that got the first
       one right by accident must not get the second one wrong in silence. */
    DCHECK(cb_cap >= FOCUS_ELEMENT_CB_SLOTS,
           "§6.6.7 step 5.11.3's focusing-steps request was handed a buffer narrower than step_call_run's "
           "[this, func, target] shape");
    if (*phase == 0) {
        JSValue fn = focus_door_new(ctx, "focusingStepsForTarget", g_id_autofocus);
        JSValueConst argv[1];

        DCHECK(node_of(el) != NULL, "§6.6.7 step 5.11.3 was handed a focus target that is not a node");
        argv[0] = el;
        /* step_call_run DUPS both the callee and the argument into the request buffer, which is what holds them
           across the suspension — so this realm's door is released here and the parked call still owns one. */
        r = step_call_run(ctx, phase, cb, cb_cap, fn, JS_UNDEFINED, 1, argv, in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        DCHECK(r == JS_STEP_CALL, "§6.6.7 step 5.11.3's focusing-steps request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 1, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "§6.6.4's focusing steps resumed into something other than their answer");
    DCHECK(JS_IsUndefined(out),
           "§6.6.4's focusing steps answered with a value — the algorithm has no result, so a value here is a "
           "member's return leaking through the door");
    JS_FreeValue(ctx, out);
    return 0;
}

/* ---- HTML §6.6.3 The tabindex attribute — the `tabIndex` IDL attribute -------------------------------------
 *
 * ITS IDL IS `[CEReactions, ReflectSetter] attribute long tabIndex` ON HTMLOrSVGOrMathMLElement, so only the
 * SETTER reflects and the getter is the section's own algorithm. That asymmetry is why it is a member of this
 * component and not a row of core/dom/element.c's ElReflect registry — element.h states the test ("A KIND MUST
 * ANSWER BOTH DIRECTIONS FROM THE ATTRIBUTE ALONE") and names this member as one of the twenty-eight that
 * cannot. It lives HERE rather than in a file of its own because every operand of the getter's default is
 * already here: §6.6.3's tabindex value is what el_is_sequentially_focusable branches on, and §4.11.2's
 * summary-for-parent-details is what el_is_ua_focusable asks.
 *
 * WHY A MIRROR IS A WRONG ANSWER AND NOT A LENIENT ONE. `<div>` has no `tabindex` attribute and its `tabIndex`
 * is −1; `<a>`'s is 0; `<input tabindex="x">` is 0, not the string. A bundle's `if (el.tabIndex >= 0)` — the
 * ordinary way a focus-trap library collects candidates — reads `undefined` on an absent member and takes its
 * false arm for every element in the document, which is a whole region of the page's own code never explored.
 *
 * THE MIXIN'S OTHER HOME IS HONESTLY ABSENT. HTMLOrSVGOrMathMLElement is included by SVGElement and
 * MathMLElement as well, and neither interface exists in this engine (SVGElement is a name on
 * browser/platform_names.h, so reading it is the ReferenceError that names the component to write). The
 * getter's default therefore never sees an SVG `a` or a MathML `a`, which is why those two of §6.6.3's arms are
 * not written below: they are unreachable through the only prototype this member is installed on, and a branch
 * for a receiver that cannot arrive is a claim this file cannot make. */

/* §6.6.3's LAST GETTER STEP, as its own predicate: "Return 0 if this is an a, area, button, frame, iframe,
   input, object, select, textarea, or SVG a element, or MathML a element, or is a summary element that is a
   summary for its parent details; otherwise −1."
   NOTE WHAT IS NOT CONDITIONAL HERE. `a` and `area` are in this list with NO `href` requirement and `input`
   with no exclusion of the Hidden type — unlike the focusability list above, which is the whole reason the two
   lists are two functions. `frame` has no interface row in core/html/html_element.c and is therefore an
   HTMLUnknownElement, which still inherits HTMLElement and so still reaches this member: the answer is decided
   by the LOCAL NAME the section names, never by which interface the element wears. */
static bool el_tabindex_default_is_zero(const lxb_dom_node_t *n)
{
    static const char *const ZERO[] = { "a", "area", "button", "frame", "iframe", "input", "object",
                                        "select", "textarea" };
    size_t i;

    for (i = 0; i < sizeof ZERO / sizeof ZERO[0]; i++)
        if (el_is(n, ZERO[i])) return true;
    return el_is_summary_for_parent_details(n);
}

/* §6.6.3's GETTER STEPS: "1. Let attribute be this's tabindex attribute. 2. If attribute is not null: 1. Let
   parsedValue be the result of integer parsing attribute's value. 2. If parsedValue is not an error and is
   WITHIN THE LONG RANGE, then return parsedValue. 3. Return 0 if this is [the list above]; otherwise −1."
   THE LONG-RANGE TEST IS NOT tabindex_value's TEST, which is why this parses rather than calling it. That
   helper answers §6.6.3's *tabindex value* — a number whose SIGN is all its two callers read — so it maps an
   overflowing digit run onto LLONG_MIN/LLONG_MAX to keep the sign, which is right for them and wrong here:
   step 2.2 sends a value outside the `long` range to step 3's DEFAULT, so `<div tabindex="99999999999">`
   reads −1 and `<a tabindex="99999999999">` reads 0. Two questions, two answers, one parse shared. */
static JSValue js_tab_index_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    const lxb_char_t *v;
    size_t len = 0;
    HtmlInteger parsed;

    (void)magic;
    /* WEB IDL §3.7.6 Attributes' brand check, a THROW and not an assert: this accessor sits on
       HTMLElement.prototype and
       a page reaches one off a prototype with `.call` on anything at all. */
    if (!html_element_is(this_val))
        return JS_ThrowTypeError(ctx, "HTMLElement.tabIndex was reached on something that is not an HTML "
                                      "element");
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "html_element_is admitted a value with no element node behind it");
    v = el_attr(n, "tabindex", &len);                                                    /* steps 1-2 */
    if (v && html_parse_integer((const char *)v, len, &parsed) && !parsed.overflow &&
        parsed.value >= INT32_MIN && parsed.value <= INT32_MAX)
        return JS_NewInt32(ctx, (int32_t)parsed.value);                                  /* step 2.2 */
    return JS_NewInt32(ctx, el_tabindex_default_is_zero(n) ? 0 : -1);                     /* step 3 */
}

/* `[ReflectSetter]` on a `long`, which HTML §2.6.1 Reflecting content attributes in IDL attributes states in
   full: "The setter steps are: … Run this's set the content attribute with the given value converted to the
   shortest possible string representing the number as a valid integer." (Its first setter step — the
   "IndexSizeError" throw — belongs to a reflection LIMITED TO ONLY NON-NEGATIVE NUMBERS, which §6.6.3's is
   not: `el.tabIndex = -1` is the single most common assignment this member ever receives.)
   `%d` over the `long` conversion's own result IS that shortest string: the conversion has already produced a
   value in the 32-bit signed range, and the shortest valid integer for one is its decimal digits with a `-`
   for a negative and no `+`, no leading zero and no exponent. */
static JSValue js_tab_index_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    char buf[16];
    int32_t v = 0;
    int wrote;

    (void)magic;
    if (!html_element_is(this_val))
        return JS_ThrowTypeError(ctx, "HTMLElement.tabIndex was set on something that is not an HTML element");
    DCHECK(JS_IsNumber(val),
           "§6.6.3's tabIndex setter was handed something that is not a number — it is declared IDL_LONG, so "
           "Web IDL's `long` conversion produced one before this body was entered");
    JS_ToInt32(ctx, &v, val);
    wrote = snprintf(buf, sizeof buf, "%d", (int)v);
    /* The widest a 32-bit signed decimal can be is "-2147483648", eleven characters, so a truncation here is
       not a small buffer — it is a value that did not come through the `long` conversion, and the attribute
       written would be a DIFFERENT NUMBER from the one assigned rather than an absent one. */
    DCHECK(wrote > 0 && (size_t)wrote < sizeof buf,
           "§6.6.3's tabIndex setter could not serialise its `long` — the value is not one the Web IDL `long` "
           "conversion can produce");
    element_attr_set(ctx, this_val, "tabindex", buf);
    return JS_UNDEFINED;
}

/* HTML §6.6.6 Focus management APIs' `Window.blur()`, WHOSE METHOD STEPS ARE "TO DO NOTHING" — the standard's
   own words, and the note beside them says why: "historically, the focus() and blur() methods actually affected
   the system-level focus of the system widget that contained the navigable, but hostile sites widely abuse this
   behavior to the user's detriment". So this is the SPEC's no-effect and not this engine's, which is the one
   shape §NO STUBS admits: a dedicated documented no-effect where the spec ITSELF states the steps are to do
   nothing. `Window.focus()` is NOT one of these — §6.6.6 gives it real steps, which focus_step runs.

   IT LIVED IN core/frame/window.c, BESIDE THE WINDOW'S OTHER MEMBERS, AND THAT PLACEMENT HAD A REASON THIS DIFF
   OVERTURNS. focus.h stated it: a member whose steps are "do nothing" has no §6.6.4 algorithm to own, so it
   belonged with the interface rather than with the section. That was true while §7.2.2's global was the ONE
   place a Window member is installed. It is not any more: HTML §7.2.1.3.1 CrossOriginProperties names `focus`
   AND `blur` among the thirteen cross-origin accessible window property names, so both must ALSO be own
   properties of §7.2.3's WindowProxy surface — and a two-member list installed from two files is the
   hand-copied list that drifts, with the drift invisible because a name missing from one surface answers
   `undefined` there rather than crashing. §6.6.6 defines both members; ONE function installs both, wherever
   they go. */
static JSValue js_win_blur(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argv; (void)magic;
    /* §6.6.6 declares `undefined blur()` — no arguments, ever, so a body that could be handed one is a body
       whose declaration and whose member have come apart. A non-variadic member's count is min(passed,
       declared) (core/idl_args.c), so `window.blur(1)` still arrives here as zero and the only way this fires
       is a declaration that stopped matching the IDL. */
    DCHECK(argc == 0,
           "HTML §6.6.6 Focus management APIs declares `undefined blur()` with no arguments and this body was "
           "reached with one — the declaration in focus_init and the IDL have come apart");
    (void)argc;
    return JS_UNDEFINED;
}

/* ---- declaration and install -------------------------------------------------------------------------------- */

void focus_init(JSContext *ctx)
{
    /* §6.6.6's `dictionary FocusOptions { boolean preventScroll = false; boolean focusVisible; }` — the second
       member carries NO default, which is the distinction step 3 reads ("if options["focusVisible"] is true, OR
       DOES NOT EXIST but in an implementation-defined way the user agent determines it would be best"). */
    /* IN §3.2.17's READ ORDER, which is LEXICOGRAPHIC over the members' identifiers and not the order the IDL
       block writes them in — the conversion reads in declared order, and a page pins that order by throwing
       from one member's getter. So `focusVisible` is declared first although §6.6.6's IDL writes it second. */
    static const IdlDictMember FOCUS_OPTIONS[] = {
        { "focusVisible",  IDL_BOOLEAN_NO_DEFAULT, false, NULL, 0 },
        { "preventScroll", IDL_BOOLEAN,            false, NULL, 0 },
    };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    /* §6.6.7 step 5.11.3's focus target, handed to the internal door as its one argument. IDL_ANY because it is
       an element WRAPPER passing between two pieces of this engine and not a value a page ever supplies — the
       args machine converts nothing and runs none of the page's code on it, and focus_step's own entry asserts
       what it is. */
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };

    DCHECK(!g_ready, "focus_init ran twice — the members are declared once per agent");
    g_focus_slot = realm_value_declare(ctx, "HTML §6.6.2 focused area of the document");
    g_id_el_focus = idl_method_id_step(ctx, ONE_DICT, 1, FOCUS_OPTIONS,
                                       (int)(sizeof FOCUS_OPTIONS / sizeof FOCUS_OPTIONS[0]),
                                       &EL_FOCUS_STEP, FOCUS_EL_FOCUS);
    idl_optional_from(0);
    g_id_el_blur = idl_method_id_step(ctx, NULL, 0, NULL, 0, &EL_BLUR_STEP, FOCUS_EL_BLUR);
    g_id_win_focus = idl_method_id_step(ctx, NULL, 0, NULL, 0, &WIN_FOCUS_STEP, FOCUS_WIN_FOCUS);
    /* §6.6.6's OTHER Window member. It is a plain body and not a step machine because its steps are to do
       nothing: there is no page code to run and so nothing to rest on. The magic stays 0 — what distinguishes
       it from any other no-effect is which BODY runs, never which number it was handed. */
    g_id_win_blur = idl_method_id(ctx, NULL, 0, js_win_blur, 0);
    /* HTML §8.1.7.3 step 17's and §6.6.7 step 5.11.3's entries. Declared with the members although they are
       installed on nothing, because what a declaration buys — the stage labels, the pool's name, the one mint —
       is what makes a parked fixup say where it is parked, and that has nothing to do with a page being able to
       see it. */
    g_id_viewport = idl_method_id_step(ctx, NULL, 0, NULL, 0, &VIEWPORT_STEP, FOCUS_VIEWPORT);
    g_id_autofocus = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &AUTOFOCUS_STEP, FOCUS_AUTOFOCUS);
    g_id_has_focus = idl_method_id(ctx, NULL, 0, js_has_focus, 0);
    /* §6.6.3's `[CEReactions, ReflectSetter] attribute long tabIndex` — the `long` conversion (ToNumber, then
       modulo 2^32 signed) runs on the page's own `valueOf` before the body below is entered. */
    g_id_set_tab_index = idl_setter_id(ctx, IDL_LONG, false, js_tab_index_set, 0);
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. It declared NOTHING, and a
       component holding a realm slot and a pool entry per member while declaring none of them is character-for-
       character the report a component holding nothing produces. The row is `document`, not this file's name:
       a sub-component names the row whose RELEASE reaches it, and document_agent_free is what reaches
       focus_free. Each `what` names §6.6's own standard for the same reason — the line is read out of a report
       headed by document's row. */
    agent_state_id("document", &g_focus_slot,
                   "the per-realm slot HTML §6.6.2 Data model's FOCUSED AREA OF THE DOCUMENT record lives in");
    agent_state_id("document", &g_id_el_focus,
                   "HTML §6.6.6 Focus management APIs' HTMLOrSVGOrMathMLElement `focus(options)` declaration");
    agent_state_id("document", &g_id_el_blur,
                   "HTML §6.6.6 Focus management APIs' HTMLOrSVGOrMathMLElement `blur()` declaration");
    agent_state_id("document", &g_id_win_focus,
                   "HTML §6.6.6 Focus management APIs' `Window.focus()` declaration");
    agent_state_id("document", &g_id_win_blur,
                   "HTML §6.6.6 Focus management APIs' `Window.blur()` declaration");
    agent_state_id("document", &g_id_has_focus,
                   "HTML §6.6.6 Focus management APIs' `Document.hasFocus()` declaration");
    agent_state_id("document", &g_id_set_tab_index,
                   "HTML §6.6.3 The tabindex attribute's `tabIndex` setter declaration");
    agent_state_id("document", &g_id_viewport,
                   "HTML §8.1.7.3 update the rendering step 17's entry into §6.6.4's focusing steps");
    agent_state_id("document", &g_id_autofocus,
                   "HTML §6.6.7 The autofocus attribute step 5.11.3's entry into §6.6.4's focusing steps");
    g_ready = 1;
}

void focus_install_document_members(JSContext *ctx, JSValueConst proto)
{
    JSValue rec;

    DCHECK(g_ready, "§6.6's Document members were installed before focus_init ran");
    /* THIS REALM'S INITIAL FOCUSED AREA — the VIEWPORT, which the record spells JS_NULL. Built here, with the
       realm, so it belongs to the pre-boot BASELINE: a record made on first touch would be made inside
       whichever flow happened to read first, and would then be that flow's document rather than the baseline
       every flow forks from. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "focus: this realm's §6.6.2 focused-area record could not be allocated");
    JS_SetPropertyStr(ctx, rec, "el", JS_NULL);
    realm_value_set(ctx, g_focus_slot, rec);

    idl_install_accessor(ctx, proto, "activeElement", js_active_element, 0, -1);
    idl_install_method(ctx, proto, "hasFocus", g_id_has_focus);
}

void focus_install_shadow_root_members(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§6.6's ShadowRoot member was installed before focus_init ran");
    idl_install_accessor(ctx, proto, "activeElement", js_active_element, 0, -1);
}

void focus_install_html_members(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§6.6's HTMLElement members were installed before focus_init ran");
    idl_install_method(ctx, proto, "focus", g_id_el_focus);
    idl_install_method(ctx, proto, "blur", g_id_el_blur);
    /* The same mixin's `tabIndex`, on THIS realm's prototype like every other member. */
    idl_install_accessor(ctx, proto, "tabIndex", js_tab_index_get, 0, g_id_set_tab_index);
}

/* §6.6.6's TWO Window members, ON ONE OBJECT — and the object is a PARAMETER because there are two of them.
 *
 * §7.2.2's global is one: Web IDL §3.7.3 Interface prototype object makes every member of a [Global] interface
 * an OWN property of the global itself. HTML §7.2.3's WindowProxy surface is the other, and it is not a
 * convenience: §7.2.1.3.1 CrossOriginProperties names `focus` and `blur` among the thirteen cross-origin
 * accessible window property names, and §7.2.3.5's cross-origin branch answers those names by letting the walk
 * reach that surface — so a name absent from it answers `undefined` across origins where §7.2.1.3.4
 * CrossOriginGetOwnPropertyHelper returns "an anonymous built-in function, created in the current realm, that
 * performs the same steps as the IDL operation P on object O". `otherW.focus()` then threw a TypeError instead
 * of running the steps, and nothing said so, because `undefined` is a plausible value for a member.
 *
 * ONE FUNCTION FOR BOTH SURFACES, which is the whole reason it takes a target. Two install sites naming two
 * members each is four lines that drift in pairs, and the drift is silent in exactly the direction that
 * matters: a member missing from the proxy surface is `undefined` there while the global still answers, so one
 * spelling of one member behaves differently from the other with no crash anywhere.
 *
 * §6.6.6's STEPS ARE THE RECEIVER'S, WHICH IS WHAT MAKES ONE FUNCTION OBJECT PER REALM SOUND. `Window.focus()`
 * reads `this`'s navigable (see the FOCUS_WIN_FOCUS arm of focus_step), so the copy installed on the reading
 * realm's proxy surface focuses the window it was CALLED ON — which is §7.2.1.3.4's "created in the current
 * realm … on object O" exactly. A member that read its own realm instead could not be installed here at all. */
void focus_install_window_members(JSContext *ctx, JSValueConst target)
{
    DCHECK(g_ready, "§6.6's Window members were installed before focus_init ran");
    idl_install_method(ctx, target, "focus", g_id_win_focus);
    idl_install_method(ctx, target, "blur", g_id_win_blur);
}

/* RELEASED BY ITS DECLARER — §6.6's focused area is declared from document_init, so document_agent_free gives
   it back. The RECORDS are the realms', released with their contexts; what a C static holds for the agent is
   the realm slot id, the six pool entries and the latch. Every one of them goes, because a release that gives
   its state back and keeps the number is core/agent_state.h's fetch_free: the only reader is the next agent's
   init, and it reads it to decide that it need not run. */
void focus_free(void)
{
    DCHECK(g_ready, "§6.6's focus machinery was released in an agent that never declared it");
    g_ready = 0;
    g_focus_slot = -1;
    g_id_el_focus = g_id_el_blur = g_id_win_focus = g_id_has_focus = g_id_set_tab_index = -1;
    g_id_win_blur = -1;
    g_id_viewport = g_id_autofocus = -1;
}
