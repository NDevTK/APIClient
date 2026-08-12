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
 * §6.6's visibility state already use, for the two reasons stated there: the record is unreachable from the
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
 * FIRING A FOCUS EVENT NEEDS AN INTERFACE THIS TREE HAS NOT GOT. §6.6.4's "fire a focus event" fires "using
 * FocusEvent, with the relatedTarget attribute initialized to r, the view attribute initialized to t's node
 * document's relevant global object, and the composed flag set". This engine has no FocusEvent and no UIEvent —
 * create_event.c's row for it has no maker, and asserts that the interface is not exposed — so the mint DFAILs
 * and names it. Firing a plain Event instead would be worse than the crash: a listener reading `e.relatedTarget`
 * would get undefined where the spec computes a real value, and a page that distinguishes a real focus event
 * from a synthetic one would be told the wrong thing. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/frame/window_proxy.h"
#include "core/html/focus.h"
#include "core/html/html_form.h"
#include "core/html/html_iframe.h"
#include "core/html/user_activation.h"

/* §6.6.2's FOCUSABLE AREAS AND THE OBJECTS §6.6.4 PASSES AROUND BESIDE THEM. A focus target is not always a
   focusable area — the focusing steps take an element that is not one, or a navigable — and a focus CHAIN holds
   Documents as well as areas. The kind travels WITH the value because the value alone cannot say: the viewport
   of a Document and the Document itself are the same node, and they are two different entries of the chain
   that fire two different events (a viewport fires none; a Document fires at its Window). */
enum { FA_NONE = 0, FA_ELEMENT, FA_VIEWPORT, FA_DOCUMENT, FA_NAVIGABLE };

static int g_focus_slot = -1;
static int g_id_el_focus = -1, g_id_el_blur = -1, g_id_win_focus = -1, g_id_has_focus = -1;
static int g_ready;

/* ---- §6.6.2's data model: the focused area of the document -------------------------------------------------
 *
 * The record holds ONE field, `el`: the focused area's element wrapper, or JS_NULL for THE VIEWPORT. Those are
 * the only two focusable areas this engine models (see the file header), so the field is total — there is no
 * third state and no "none": a Document with a browsing context always has a focused area, and a Document
 * WITHOUT one has no realm record at all, which is how §6.6.6's getter answers null for
 * `implementation.createHTMLDocument("")`. */

/* THE REALM WHOSE ACTIVE DOCUMENT `doc` IS, or NULL. A second Document in a realm (createHTMLDocument, a
   DOMParser parse) has a record but is not the realm's active document, and §6.6.2's focused area is the ACTIVE
   document's — so a receiver that is not it has none, which is a real answer and not a missing one. */
static JSContext *active_document_realm(const lxb_dom_node_t *doc)
{
    JSContext *realm;

    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT) return NULL;
    realm = document_realm_of(doc);
    if (!realm) return NULL;
    return node_of(document_object(realm)) == (lxb_dom_node_t *)doc ? realm : NULL;
}

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

/* HTML §2.3.4.1's RULES FOR PARSING INTEGERS, over the bytes of a content attribute. `*pv` is written only on
   success; the answer is whether the string IS an integer, which is exactly what §6.6.3's "the tabindex value
   of an element ... if parsing fails or the attribute is not specified, then the tabindex value is null" needs
   and what a plain atoi cannot say (atoi("x") is 0, which is a tabindex that makes an element focusable). */
static bool parse_integer(const lxb_char_t *s, size_t len, long *pv)
{
    size_t i = 0;
    bool negative = false;
    long v = 0;

    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
    if (i >= len) return false;
    if (s[i] == '-') { negative = true; i++; }
    else if (s[i] == '+') { i++; }
    if (i >= len || s[i] < '0' || s[i] > '9') return false;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (s[i] - '0');
    *pv = negative ? -v : v;
    return true;
}

/* §6.6.3's TABINDEX VALUE of an element: the parsed attribute, or null. */
static bool tabindex_value(const lxb_dom_node_t *n, long *pv)
{
    size_t len = 0;
    const lxb_char_t *v = el_attr(n, "tabindex", &len);

    return v && parse_integer(v, len, pv);
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
   or being used as relevant canvas fallback content". Being rendered is "has any associated CSS layout boxes",
   and this engine computes no layout — so what decides it here are the two facts the TREE carries: the element
   is CONNECTED (a node whose root is not a document has no boxes in any user agent), and neither it nor an
   ancestor carries the `hidden` content attribute, whose UA-stylesheet rule
   (§15.3.1) is `display: none`. A `display:none` written in author CSS or in an inline style is a computed
   value the CSSOM in this build exposes to no C caller, so it does not participate; that is a narrower answer
   than a laying-out browser's, never a wider one — an element this says is being rendered may not be, and one
   it says is not never is. */
static bool el_is_being_rendered(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;
    size_t len = 0;

    if (!node_is_connected(n)) return false;
    for (a = n; a; a = a->parent)
        if (a->type == LXB_DOM_NODE_TYPE_ELEMENT && el_attr(a, "hidden", &len)) return false;
    return true;
}

/* §6.6.3's "MODULO PLATFORM CONVENTIONS, it is SUGGESTED that the following elements should be considered as
   focusable areas" — the standard's own list, which is what "determined by the user agent to be focusable"
   means for a user agent with no platform conventions of its own to differ by. Every entry is a fact about the
   element's markup, so every entry is decidable here. */
static bool el_is_ua_focusable(const lxb_dom_node_t *n)
{
    size_t len = 0;

    if (el_is(n, "a") || el_is(n, "area")) return el_attr(n, "href", &len) != NULL;
    if (el_is(n, "button") || el_is(n, "select") || el_is(n, "textarea")) return true;
    if (el_is(n, "input")) {
        const lxb_char_t *t = el_attr(n, "type", &len);
        return !(t && len == 6 && !strncasecmp((const char *)t, "hidden", 6));
    }
    if (el_is(n, "summary")) {
        /* "summary elements that are the FIRST summary element child of a details element". */
        const lxb_dom_node_t *p = n->parent, *c;
        if (!el_is(p, "details")) return false;
        for (c = p->first_child; c; c = c->next)
            if (el_is(c, "summary")) return c == n;
        return false;
    }
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
    long ti;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (!tabindex_value(n, &ti) && !el_is_ua_focusable(n)) return false;
    /* "the element is either not a shadow host, or has a shadow root whose DELEGATES FOCUS is false" — a host
       that delegates is not itself a focusable area, which is what sends §6.6.4 to the focus delegate. */
    sr = shadow_root_of_element(ctx, lxb_dom_interface_element(n));
    if (sr && shadow_root_flag(ctx, sr, SHADOW_ROOT_DELEGATES_FOCUS)) return false;
    if (html_form_control_is_disabled(ctx, el)) return false;   /* "not actually disabled" */
    if (node_is_inert(n)) return false;
    return el_is_being_rendered(n);
}

/* §6.6.2's SEQUENTIALLY FOCUSABLE, as this user agent determines it: a focusable area is in its Document's
   sequential focus navigation order unless its tabindex value is a NEGATIVE INTEGER, which §6.6.3 states as
   "the user agent must consider the element as a focusable area, but should omit the element from any
   tabindex-ordered focus navigation scope". A user agent may narrow it further for its user's sake (macOS skips
   non-form controls); this one has no user to have a preference. */
static bool el_is_sequentially_focusable(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);
    long ti;

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
        JSContext *realm = active_document_realm(lxb_dom_interface_node(n->owner_document));
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
            realm = active_document_realm(doc);
            if (!realm) break;
            JS_FreeValue(ctx, cur);
            cur = JS_DupValue(realm, document_object(realm));
            k = FA_DOCUMENT;
            continue;
        }
        DCHECK(k == FA_DOCUMENT, "a focus chain walked into an entry that is neither an area nor a Document");
        realm = active_document_realm(n);
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

/* THE ONE THING §6.6 NEEDS THAT THIS TREE HAS NOT GOT. "To fire a focus event named e at an element t with a
   given related target r, fire an event named e at t, USING FocusEvent, with the relatedTarget attribute
   initialized to r, the view attribute initialized to t's node document's relevant global object, and the
   COMPOSED FLAG SET." Three of those four — the interface, `relatedTarget` and `view` — are FocusEvent's, and
   FocusEvent inherits UIEvent, and this engine has neither: create_event.c carries a row for FocusEvent with no
   maker and asserts that the interface is not exposed.
   A plain Event would be a WRONG answer rather than a partial one — `e.relatedTarget` is what a page reads to
   learn where the focus went, and `e.composed` is what decides whether the event escapes a shadow tree at all —
   so the mint aborts and names the component to build. */
static JSValue fire_a_focus_event_new(JSContext *ctx, const char *type, bool bubbles, JSValueConst related)
{
    (void)ctx; (void)type; (void)bubbles; (void)related;
    DFAIL("HTML §6.6.4's FIRE A FOCUS EVENT needs the FocusEvent interface, which this engine does not have: "
          "`interface FocusEvent : UIEvent { constructor(DOMString type, optional FocusEventInit init); "
          "readonly attribute EventTarget? relatedTarget; }` (UI Events §3.3.1), over `interface UIEvent : "
          "Event { readonly attribute Window? view; readonly attribute long detail; }` (UI Events §3.2.1). "
          "Build core/events/ui_event.c and core/events/focus_event.c, fill create_event.c's `focusevent` row "
          "with the maker (its DCHECK asserts the row and the exposed interface agree), and mint the event "
          "here with relatedTarget, view = the target's node document's relevant global object, and the "
          "composed flag set");
    return JS_UNDEFINED;
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
    realm = active_document_realm(doc);
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
    target = active_document_realm(n);
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
    X(FOC_ENTER,    "HTML §6.6.6 focus()/blur() step 1 (the allow focus steps; the algorithm's focus target)") \
    X(FOC_UNFOCUS,  "HTML §6.6.4 unfocusing steps steps 1-7 (the delegating host, inert, the old chain, and " \
                    "the branch on the top document's system focus)") \
    X(FOC_AREA,     "HTML §6.6.4 focusing steps steps 1-5 (get the focusable area over the focus delegate " \
                    "search, the fallback target, the navigable container, inert, and the already-focused " \
                    "early return — one stage because none of them can reach the page's code)") \
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

/* The four entry points, as the magic each declaration carries. */
enum { FOCUS_EL_FOCUS = 0, FOCUS_EL_BLUR, FOCUS_WIN_FOCUS };

typedef struct {
    uint8_t  fphase;                 /* the fire request's own phase */
    uint8_t  kind;                   /* the current focus target's kind */
    JSValue  target;                 /* the current focus target (owned) */
    JSValue  old_chain, new_chain;   /* Arrays of alternating (kind, value) (owned) */
    uint32_t old_n, new_n;           /* how many ENTRIES of each are live — step 1's pop shortens both */
    uint32_t i, j;                   /* step 2's cursor, and step 4's count of entries already visited */
    JSValue  ev;                     /* the event being fired (owned) */
    JSValue  cb[4];                  /* the fire request's buffer: [this, dispatch, target, event] */
} FocusState;

static void focus_state_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FocusState *s = st;
    int k;

    v->val(ctx, &s->target);
    v->val(ctx, &s->old_chain);
    v->val(ctx, &s->new_chain);
    v->val(ctx, &s->ev);
    for (k = 0; k < 4; k++) v->val(ctx, &s->cb[k]);
}

static void focus_state_release(JSContext *ctx, void *st)
{
    FocusState *s = st;
    int k;

    JS_FreeValue(ctx, s->target);
    JS_FreeValue(ctx, s->old_chain);
    JS_FreeValue(ctx, s->new_chain);
    JS_FreeValue(ctx, s->ev);
    s->target = s->old_chain = s->new_chain = s->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
}

/* Every owned field in place before the first thing that can fail — the failure path tears this state down
   through focus_state_release, which frees exactly what the state holds and nothing else. */
static bool focus_state_init(JSContext *ctx, FocusState *s)
{
    int k;

    s->target = s->ev = JS_UNDEFINED;
    s->old_chain = s->new_chain = JS_UNDEFINED;
    s->kind = FA_NONE;
    s->old_n = s->new_n = s->i = s->j = 0;
    s->fphase = 0;
    for (k = 0; k < 4; k++) s->cb[k] = JS_UNDEFINED;
    s->old_chain = JS_NewArray(ctx);
    s->new_chain = JS_NewArray(ctx);
    return !JS_IsException(s->old_chain) && !JS_IsException(s->new_chain);
}

/* §6.6.6's ALLOW FOCUS STEPS, given a Document target.
     "If target is allowed to use the `focus-without-user-activation` feature, return true."  That feature's
   DEFAULT ALLOWLIST is 'self' (HTML §2.2.3's policy-controlled feature list), so a document is allowed to use
   it exactly while it is same-origin with the top-level traversable's active document — this engine parses no
   `Permissions-Policy` header and no `allow` attribute, so nothing narrows or widens the default and the
   default IS the answer.
     "If target's relevant global object has TRANSIENT ACTIVATION, return true."  Asked of §6.4.1's real state
   (core/html/user_activation.c), never assumed: it answers false today only because this engine dispatches no
   trusted activation triggering input event, and it starts answering true the moment one exists — which is the
   whole reason that component is a state machine rather than the constant it replaced. */
static bool allow_focus_steps(JSContext *target)
{
    JSValue top = window_proxy_top_navigable(target, document_window_proxy(target));
    bool same = JS_IsObject(top) && window_proxy_same_origin_of(top);

    JS_FreeValue(target, top);
    return same || user_activation_transient(target);
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
                   trigger — step 5's is a user-agent presentation with no scriptable result. */
                if (!allow_focus_steps(ctx)) return 0;
                s->kind = FA_NAVIGABLE;
                s->target = JS_DupValue(ctx, document_window_proxy(ctx));
                hdr->stage = FOC_AREA;
                continue;
            }
            /* WEB IDL §3.7.5's brand check — a TypeError, not an assert, because
               `HTMLElement.prototype.focus.call(null)` is a thing the corpus does deliberately. */
            n = node_of(hdr->this_val);
            if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                JS_ThrowTypeError(ctx, "this is not an element");
                return -1;
            }
            s->kind = FA_ELEMENT;
            s->target = JS_DupValue(ctx, hdr->this_val);
            if (magic == FOCUS_EL_BLUR) {
                hdr->stage = FOC_UNFOCUS;
                continue;
            }
            DCHECK(magic == FOCUS_EL_FOCUS, "the focus machine was declared with a magic §6.6.6 does not name");
            /* §6.6.6's focus(options) step 1. Steps 3 and 4 are decided here, where the OPTIONS are, and both
               are answered rather than deferred:
                 step 3's "indicate focus" is the focus ring — a user-agent presentation with no scriptable
                   result, which is the one shape §NO STUBS permits as a documented no-effect;
                 step 4's "scroll a target into view" needs a SCROLLING BOX, and this build has none — asserted
                   the way rendering.c asserts its own unwritten steps, against the producer that would make one
                   exist, so the day CSSOM-View's scrolling arrives this fires and names the step to write. */
            {
                JSValue g = JS_GetGlobalObject(ctx), scroll;
                JSAtom a = JS_NewAtom(ctx, "scrollTo");
                int has;

                CHECK(a != JS_ATOM_NULL, "focus: the `scrollTo` producer name could not be interned");
                has = JS_GetOwnSlot(ctx, &scroll, g, a);
                JS_FreeAtom(ctx, a);
                if (has > 0) JS_FreeValue(ctx, scroll);
                JS_FreeValue(ctx, g);
                DCHECK(has <= 0 || idl_dict_bool(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "preventScroll"),
                       "HTML §6.6.6 focus() step 4 SCROLLS A TARGET INTO VIEW (CSSOM VIEW) given this, "
                       "\"auto\", \"center\" and \"center\" — this build now has a way to scroll a scrolling "
                       "box, so step 4 must be written here");
            }
            /* Step 1's allow focus steps are given THIS'S NODE DOCUMENT, not the running realm's — an element
               adopted into another same-origin document is focused under that document's policy. A document
               with no browsing context has neither the feature nor an activation, so its elements are not
               focusable through this member at all. */
            doc = n->owner_document ? active_document_realm(lxb_dom_interface_node(n->owner_document)) : NULL;
            if (!doc || !allow_focus_steps(doc)) return 0;
            hdr->stage = FOC_AREA;
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
                doc_realm = active_document_realm(node_of(last));
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
                hdr->stage = FOC_AREA;
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
            hdr->stage = FOC_CHAINS;
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
            hdr->stage = FOC_CHANGE;
            continue;
        }

        case FOC_CHANGE: {
            /* Step 2.1. Its condition is "the USER has changed the element's value or its list of selected
               files while the control was focused WITHOUT COMMITTING that change" — a user edit, which this
               user agent has no input device to make, and which a script's own `input.value = x` explicitly is
               not (no browser fires `change` for one either). So no old-chain entry meets it and no `change`
               is fired; the stage still exists because the day an input device is modelled, this is where its
               uncommitted edit is committed. */
            if (s->i >= s->old_n) { hdr->stage = FOC_FOCUS; continue; }
            hdr->stage = FOC_BLUR;
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
                if (blur) { hdr->stage = FOC_FOCUSOUT; continue; }
                s->i++;
                hdr->stage = FOC_CHANGE;
                return JS_STEP_YIELD;   /* one chain entry per step */
            }
            if (JS_IsUndefined(s->ev)) {
                JSValue related = chain_related(ctx, s->new_chain, s->new_n,
                                                kind == FA_ELEMENT && s->i + 1 == s->old_n);
                s->ev = fire_a_focus_event_new(ctx, blur ? "blur" : "focusout", !blur, related);
                JS_FreeValue(ctx, related);
                if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, entry); return -1; }
            }
            if (JS_IsUndefined(s->ev)) {
                /* A release build, where the mint's DFAIL is compiled out and there is no interface to fire
                   with. Nothing is dispatched — a plain Event here would report a focus event that carries
                   neither its related target nor its composed flag. */
                JS_FreeValue(ctx, entry);
                if (blur) { hdr->stage = FOC_FOCUSOUT; continue; }
                s->i++;
                hdr->stage = FOC_CHANGE;
                return JS_STEP_YIELD;
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
            if (blur) { hdr->stage = FOC_FOCUSOUT; continue; }
            s->i++;
            hdr->stage = FOC_CHANGE;
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
                JSContext *realm = active_document_realm(doc);

                DCHECK(realm != NULL, "§6.6.4 step 4.1.2 designates the focused area of a document that is no "
                                      "realm's active document");
                if (realm) {
                    /* Step 4.1.1 sets the document's relevant global object's NAVIGATION API's "focus changed
                       during ongoing navigation" — this engine has no Navigation API (no `window.navigation`),
                       so there is no such flag to set and none is invented. */
                    focused_area_designate(realm, kind == FA_VIEWPORT ? JS_NULL : entry);
                }
            }
            tgt = chain_event_target(kind, entry);
            if (!JS_IsObject(tgt)) {
                JS_FreeValue(ctx, entry);
                if (focus) { hdr->stage = FOC_FOCUSIN; continue; }
                s->j++;
                return JS_STEP_YIELD;   /* one chain entry per step */
            }
            if (JS_IsUndefined(s->ev)) {
                JSValue related = chain_related(ctx, s->old_chain, s->old_n,
                                                kind == FA_ELEMENT && at + 1 == s->new_n);
                s->ev = fire_a_focus_event_new(ctx, focus ? "focus" : "focusin", !focus, related);
                JS_FreeValue(ctx, related);
                if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, entry); return -1; }
            }
            if (JS_IsUndefined(s->ev)) {                                    /* release: see FOC_BLUR's note */
                JS_FreeValue(ctx, entry);
                if (focus) { hdr->stage = FOC_FOCUSIN; continue; }
                s->j++;
                return JS_STEP_YIELD;
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
            if (focus) { hdr->stage = FOC_FOCUSIN; continue; }
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
    focus_step, sizeof(FocusState), focus_state_visit, focus_state_release,
    "HTML §6.6.6 HTMLOrSVGOrMathMLElement.focus(options), over §6.6.4's focusing and focus update steps",
    FOCUS_STEPS
};
static const IdlStepDecl EL_BLUR_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, focus_state_release,
    "HTML §6.6.6 HTMLOrSVGOrMathMLElement.blur(), over §6.6.4's unfocusing steps",
    FOCUS_STEPS
};
static const IdlStepDecl WIN_FOCUS_STEP = {
    focus_step, sizeof(FocusState), focus_state_visit, focus_state_release,
    "HTML §6.6.6 Window.focus(), over §6.6.4's focusing and focus update steps",
    FOCUS_STEPS
};

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

    DCHECK(!g_ready, "focus_init ran twice — the members are declared once per agent");
    g_focus_slot = realm_value_declare(ctx, "HTML §6.6.2 focused area of the document");
    g_id_el_focus = idl_method_id_step(ctx, ONE_DICT, 1, FOCUS_OPTIONS,
                                       (int)(sizeof FOCUS_OPTIONS / sizeof FOCUS_OPTIONS[0]),
                                       &EL_FOCUS_STEP, FOCUS_EL_FOCUS);
    idl_optional_from(0);
    g_id_el_blur = idl_method_id_step(ctx, NULL, 0, NULL, 0, &EL_BLUR_STEP, FOCUS_EL_BLUR);
    g_id_win_focus = idl_method_id_step(ctx, NULL, 0, NULL, 0, &WIN_FOCUS_STEP, FOCUS_WIN_FOCUS);
    g_id_has_focus = idl_method_id(ctx, NULL, 0, js_has_focus, 0);
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
    idl_install_method(ctx, proto, "hasFocus", 0, g_id_has_focus);
}

void focus_install_shadow_root_members(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§6.6's ShadowRoot member was installed before focus_init ran");
    idl_install_accessor(ctx, proto, "activeElement", js_active_element, 0, -1);
}

void focus_install_html_members(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§6.6's HTMLElement members were installed before focus_init ran");
    idl_install_method(ctx, proto, "focus", 0, g_id_el_focus);
    idl_install_method(ctx, proto, "blur", 0, g_id_el_blur);
}

void focus_install_window_members(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "§6.6's Window member was installed before focus_init ran");
    idl_install_method(ctx, global, "focus", 0, g_id_win_focus);
}
