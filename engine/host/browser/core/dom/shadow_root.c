/* THE ShadowRoot INTERFACE AND "attach a shadow root" — DOM §4.8, §4.9's two Element members, and §4.2's
 * shadow-including root.
 *
 * IT WAS ABSENT ENTIRELY. `attachShadow` was not a function, `ShadowRoot` was not a global, and the engine's
 * own source said so in five places — element_internals.c refused to install `shadowRoot` because a getter that
 * could only ever answer null is the stub the IDL audit exists to expose, and node.c's `root`, event.c's
 * `composedPath` and event_target.c's dispatch each carried a sentence saying shadow trees do not exist here.
 * A page whose component library calls `attachShadow` in a constructor threw on that line, HTML §8.1.4.6's
 * report fired an `error` event, and everything the component would have built was never built — so the moat
 * this engine reports for a Web-Components page is the moat of the code that runs BEFORE the first custom
 * element mounts.
 *
 * THE NODE IS LEXBOR'S OWN. `lxb_dom_shadow_root_interface_create` allocates a node whose type is
 * LXB_DOM_NODE_TYPE_SHADOW_ROOT and which carries §4.8's `host` and `mode` in C fields — and lexbor's own
 * `lxb_dom_node_host_including_inclusive_ancestor` already climbs through it. Binding to what is there beats
 * inventing a parallel record, and the DISTINCT node type is what makes "is this a shadow root" answerable with
 * no realm in hand, which retargeting, the shadow-including root and "find a slot" all need. §4.8 still says a
 * ShadowRoot IS a DocumentFragment: `nodeType` answers 11, `nodeName` answers "#document-fragment", and every
 * rule the standard states over DocumentFragment reaches one through node_is_document_fragment.
 *
 * WHERE THE STATE LIVES, AND WHY IT IS SPLIT.
 *   - `host` and `mode` are lexbor's fields, because they are the fields lexbor's own tree algorithms read. A
 *     second copy would be two answers to one question. They are written ONCE, by "attach a shadow root", on a
 *     node the attaching flow has just created — flow-private state, which the COW delta deliberately does not
 *     capture — and nothing in §4.8 ever writes either again.
 *   - The five booleans and the slot-assignment mode live on the shadow root's WRAPPER, in an internal-slot
 *     record, for the reason element_internals.c states: an own property is captured by the heap COW delta for
 *     free and parks with the flow that wrote it.
 *   - THE ELEMENT -> SHADOW ROOT LINK IS A WRAPPER SLOT, and that is the load-bearing one. The host is a
 *     BASELINE element that two flows share, so `attachShadow` in one arm of a fork must not be visible in the
 *     other; a C pointer on the lexbor element would be one answer for every flow, which is the defect class
 *     CLAUDE.md names as one fact answered from one place for many agents. A property write is captured.
 *
 * WHAT IS HONESTLY ABSENT, BY NAME — see SPEC_STEPS.md §17.6. HTML's `DocumentOrShadowRoot` addition
 * `styleSheets`.
 * `delegatesFocus` HAS ITS EFFECT as of HTML §6.6.4: it is what makes a host NOT a focusable area (§6.6.2's
 * row 1) and what sends `get the focusable area` to the FOCUS DELEGATE, and core/html/focus.c reads it through
 * shadow_root_flag below. The mixin's other addition, `activeElement`, is installed on this prototype by the
 * same component — its getter RETARGETS the focused area against the receiver, which is why a ShadowRoot
 * answers with its own tree's node and not with the document's.
 * `ShadowRootInit`'s `customElementRegistry` is no longer among them: §4.8's registry is a real parameter of
 * "attach a shadow root", attachShadow resolves steps 1-3 (this document's registry, the member's override,
 * and the NotSupportedError for one that is neither scoped nor this document's), and step 3.1's disable-shadow
 * lookup asks the HOST ELEMENT'S registry rather than the document's — which is the only form that can refuse
 * a host inside a scoped tree, or answer nothing for one whose registry is null.
 * `declarative` has a writer as of HTML §13.2.6.4.4 — declarative_shadow.c — `clonable` has a READER as of DOM
 * §4.4 step 6, which is shadow_root_clone_onto below, and `serializable`, `delegatesFocus`, `clonable`, `mode`
 * and `slot assignment` are ALL read by HTML §13.3 step 4.2, which writes them back out as the
 * `<template shadowrootmode>` §13.2.6.4.4 reads. HTML §8.5's `partial interface ShadowRoot` — `innerHTML`,
 * `getHTML`, `setHTMLUnsafe`, `setHTML` — is installed below, the last of them over HTML §8.6's sanitizer. */
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/dom/interfaces/shadow_root.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/document_fragment.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/html/custom_elements.h"
#include "core/html/focus.h"
#include "core/html/fragment_serializer.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "solver/dom_cow.h"

static JSClassID g_sr_class;
static int       g_ready;
static int       g_id_attach = -1;
/* HTML §8.5's `partial interface ShadowRoot` — the markup members, declared once per agent like every other. */
static int       g_id_inner_get = -1, g_id_inner_set = -1, g_id_set_html_unsafe = -1, g_id_set_html = -1;

/* THE SLOT KEYS — Symbols this component minted and never published, so none of §4.8's state is a string
   property of the engine's invention sitting where `Object.keys` reports it. */
static JSValue g_slots_key = JS_UNDEFINED;    /* ShadowRoot -> its §4.8 record */
static JSAtom  g_atom_slots = JS_ATOM_NULL;
static JSValue g_shadow_key = JS_UNDEFINED;   /* Element -> its shadow root's wrapper (§4.9's association) */
static JSAtom  g_atom_shadow = JS_ATOM_NULL;

/* CONFIGURABLE AND WRITABLE for the same reason custom_elements.c's state slot is: §4.8's `declarative` is
   written twice on a declarative root's second attach, and a slot defined with no flags makes the second write
   a silent no-op. */
#define SR_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* ---- the node's own two facts ------------------------------------------------------------------------------- */

bool shadow_root_is(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_SHADOW_ROOT;
}

bool shadow_root_is_value(JSValueConst v)
{
    return shadow_root_is(node_of(v));
}

lxb_dom_element_t *shadow_root_host(const lxb_dom_node_t *n)
{
    lxb_dom_shadow_root_t *sr;

    DCHECK(shadow_root_is(n), "§4.8's host was asked of a node that is not a shadow root");
    sr = lxb_dom_interface_shadow_root((lxb_dom_node_t *)n);
    DCHECK(sr->host != NULL, "§4.8: a shadow root's host is never null, and this one's is — the host is set by "
                             "attach a shadow root, which is the only thing that makes one of these nodes");
    return sr->host;
}

bool shadow_root_is_open(const lxb_dom_node_t *n)
{
    DCHECK(shadow_root_is(n), "§4.8's mode was asked of a node that is not a shadow root");
    return lxb_dom_interface_shadow_root((lxb_dom_node_t *)n)->mode == LXB_DOM_SHADOW_ROOT_MODE_OPEN;
}

/* §4.2's SHADOW-INCLUDING ROOT: "the root's host's shadow-including root, if the object's root is a shadow
   root; otherwise its root". Iterative, because the nesting is the PAGE's — a component inside a component
   inside a component is three shadow roots deep and a C recursion here would be a page-controlled C stack. */
lxb_dom_node_t *shadow_root_shadow_including_root(lxb_dom_node_t *n)
{
    lxb_dom_node_t *r;

    DCHECK(n != NULL, "§4.2's shadow-including root was asked of no node");
    for (r = node_root(n); shadow_root_is(r); r = node_root(r))
        r = lxb_dom_interface_node(shadow_root_host(r));
    return r;
}

/* §4.2's "A is a SHADOW-INCLUDING INCLUSIVE ANCESTOR of B", which the standard states the other way round —
   B is a shadow-including descendant of A if it is a descendant, or if B's ROOT is a shadow root whose HOST is
   one. So it is decided by climbing from B: parents while there are parents, and at a shadow root the climb
   continues at that root's host. A plain ancestor walk answers `false` for every node inside a shadow tree,
   which is exactly the case each caller uses this to detect — §2.9's event path walk asks it to find the
   boundary the event must retarget at, and §4.13.7's `setValidity` asks it of an anchor element. */
bool shadow_root_is_shadow_including_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b)
{
    const lxb_dom_node_t *n;

    if (!a || !b)
        return false;
    for (n = b; n; ) {
        if (n == a)
            return true;
        if (n->parent)
            n = n->parent;
        else if (shadow_root_is(n))
            n = lxb_dom_interface_node(shadow_root_host(n));
        else
            n = NULL;
    }
    return false;
}

/* §4.2's SHADOW-INCLUDING TREE ORDER, as the one step every walk over it is made of: "tree order with the
   addition of an element's shadow root's node tree inserted JUST AFTER the element is encountered". So a node's
   successor is its shadow root if it has one, then its first child, and otherwise the climb — which, on leaving
   a shadow tree, resumes at the HOST'S OWN CHILDREN rather than at the host's next sibling, because the shadow
   tree was inserted before them.
   Iterative and explicit, for the reason shadow_root_shadow_including_root is: the nesting is the page's. NULL
   when the walk leaves `root`, so a caller writes the same `for` it writes with node_next_in. */
lxb_dom_node_t *shadow_root_next_in_shadow_including(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    lxb_dom_node_t *u, *shadow;

    DCHECK(n != NULL && root != NULL, "§4.2's shadow-including tree order walk was asked about no node");
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        shadow = shadow_root_of_element(ctx, lxb_dom_interface_element(n));
        if (shadow)
            return shadow;
    }
    if (n->first_child)
        return n->first_child;
    for (u = n; u; ) {
        /* THE ROOT TEST COMES FIRST, and it is what makes a walk rooted AT a shadow root terminate: the climb
           out of a shadow tree is a climb to the HOST, which is outside the walk when the shadow root is what
           the caller asked about. */
        if (u == root)
            return NULL;
        if (shadow_root_is(u)) {
            lxb_dom_node_t *host = lxb_dom_interface_node(shadow_root_host(u));

            if (host->first_child)
                return host->first_child;
            u = host;
            continue;
        }
        if (u->next)
            return u->next;
        u = u->parent;
    }
    return NULL;
}

/* ---- the ELEMENT -> shadow root association ------------------------------------------------------------------ */

JSValue shadow_root_of_element_wrap(JSContext *ctx, JSValueConst el_wrap)
{
    JSValue v;

    DCHECK(g_ready, "an element's shadow root was asked for before shadow_root_init ran");
    if (!JS_IsObject(el_wrap)) return JS_NULL;
    if (JS_GetOwnSlot(ctx, &v, el_wrap, g_atom_shadow) <= 0) return JS_NULL;
    DCHECK(shadow_root_is(node_of(v)), "an element's shadow-root slot holds something that is not a shadow "
                                       "root — the slot is written by attach a shadow root and by nothing else");
    return v;
}

lxb_dom_node_t *shadow_root_of_element(JSContext *ctx, const lxb_dom_element_t *el)
{
    JSValue wrap, sr;
    lxb_dom_node_t *n;

    if (!el) return NULL;
    /* THE WRAPPER IS PEEKED, NEVER MINTED. A shadow host is an element `attachShadow` was called ON, so it has
       a wrapper and the identity map holds it for as long as the node lives — an element with no wrapper has no
       shadow root, and answering that without allocating is what keeps this callable from the tree walk that
       runs on every insertion. */
    wrap = node_wrap_peek(lxb_dom_interface_node((lxb_dom_element_t *)el));
    if (!JS_IsObject(wrap)) return NULL;
    sr = shadow_root_of_element_wrap(ctx, wrap);
    n = node_of(sr);
    JS_FreeValue(ctx, sr);
    return n;
}

/* ---- §4.8's record on the shadow root's wrapper --------------------------------------------------------------- */

/* WHICH of §4.8's fields — one enum, used as the getter magic and as the record's key set, so a field cannot be
   read under one name and written under another. */
/* SR_KEEP_REGISTRY_NULL is §4.8's `keep custom element registry null`, "initially false", and DOM states the
   one thing that makes it worth a field: "this can only ever be true in combination with declarative shadow
   roots". HTML §13.2.6.4.4 is its only writer — a `<template shadowrootcustomelementregistry>` — and without
   it that attribute would be undone by the first adoption: §4.5's adopt gives a shadow root with a NULL
   registry the new document's, unless this says not to. */
enum { SR_MODE = 0, SR_DELEGATES_FOCUS, SR_SLOT_ASSIGNMENT, SR_CLONABLE, SR_SERIALIZABLE, SR_HOST,
       SR_AVAILABLE_TO_INTERNALS, SR_DECLARATIVE, SR_KEEP_REGISTRY_NULL };
static const char *const SR_FIELD[] = {
    "mode", "delegatesFocus", "slotAssignment", "clonable", "serializable", "host",
    "availableToElementInternals", "declarative", "keepCustomElementRegistryNull"
};

static JSValue sr_slots(JSContext *ctx, JSValueConst sr)
{
    JSValue v;

    if (JS_GetOwnSlot(ctx, &v, sr, g_atom_slots) <= 0) return JS_UNDEFINED;
    return v;
}

static bool sr_flag(JSContext *ctx, JSValueConst sr, int which)
{
    JSValue slots = sr_slots(ctx, sr), v;
    bool b;

    DCHECK(JS_IsObject(slots), "a §4.8 field was read off a shadow root with no record — the record is built by "
                               "attach a shadow root, which is the only thing that makes one of these");
    v = JS_GetPropertyStr(ctx, slots, SR_FIELD[which]);
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, slots);
    return b;
}

bool shadow_root_slot_assignment_is_manual(JSContext *ctx, const lxb_dom_node_t *n)
{
    JSValueConst wrap = node_wrap_peek(n);
    JSValue slots, v;
    const char *s;
    bool manual;

    DCHECK(shadow_root_is(n), "§4.8's slot assignment was asked of a node that is not a shadow root");
    DCHECK(JS_IsObject(wrap), "a shadow root has no wrapper — attach a shadow root mints one, and it is the "
                              "only thing that makes one of these nodes");
    slots = sr_slots(ctx, wrap);
    DCHECK(JS_IsObject(slots), "a shadow root has no §4.8 record");
    v = JS_GetPropertyStr(ctx, slots, SR_FIELD[SR_SLOT_ASSIGNMENT]);
    s = JS_ToCString(ctx, v);
    manual = s != NULL && strcmp(s, "manual") == 0;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, slots);
    return manual;
}

bool shadow_root_flag(JSContext *ctx, const lxb_dom_node_t *n, ShadowRootFlag which)
{
    /* THE PUBLIC NAME AND THE RECORD'S KEY ARE ONE PAIRING, stated here because they are two enums and C can
       see nothing wrong with reading one at the other's index. */
    static const int FIELD_OF[] = { SR_DELEGATES_FOCUS, SR_CLONABLE, SR_SERIALIZABLE };
    JSValueConst wrap = node_wrap_peek(n);

    DCHECK(shadow_root_is(n), "a §4.8 boolean was asked of a node that is not a shadow root");
    DCHECK(which >= 0 && which < (int)(sizeof(FIELD_OF) / sizeof(FIELD_OF[0])),
           "a §4.8 boolean was asked for under a name the field table does not have");
    DCHECK(JS_IsObject(wrap), "a shadow root has no wrapper — attach a shadow root mints one, and it is the "
                              "only thing that makes one of these nodes");
    return sr_flag(ctx, wrap, FIELD_OF[which]);
}

/* §4.8's SEVEN getters, over the receiver's record and the node's two C fields. */
static JSValue js_sr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!shadow_root_is(n))
        return JS_ThrowTypeError(ctx, "a ShadowRoot member was read on something that is not a shadow root");
    switch (magic) {
    case SR_MODE:            return JS_NewString(ctx, shadow_root_is_open(n) ? "open" : "closed");
    case SR_HOST:            return node_wrap(ctx, lxb_dom_interface_node(shadow_root_host(n)));
    case SR_SLOT_ASSIGNMENT: {
        JSValue slots = sr_slots(ctx, this_val), v;
        DCHECK(JS_IsObject(slots), "a shadow root has no §4.8 record");
        v = JS_GetPropertyStr(ctx, slots, SR_FIELD[SR_SLOT_ASSIGNMENT]);
        JS_FreeValue(ctx, slots);
        return v;
    }
    default:                 return JS_NewBool(ctx, sr_flag(ctx, this_val, magic));
    }
}

/* ---- §4.8 "attach a shadow root" ------------------------------------------------------------------------------ */

/* §4.8's VALID SHADOW HOST NAME: a valid custom element name, or one of eighteen built-ins. "This list is
   intentionally limited so that built-in elements can gain internal shadow trees over time as needed." */
static const char *const SHADOW_HOST_NAMES[] = {
    "article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6",
    "header", "main", "nav", "p", "section", "span", NULL
};

static bool sr_valid_host_name(const char *name, size_t len)
{
    int i;

    if (custom_elements_name_is_valid(name, len)) return true;
    for (i = 0; SHADOW_HOST_NAMES[i]; i++)
        if (strlen(SHADOW_HOST_NAMES[i]) == len && memcmp(SHADOW_HOST_NAMES[i], name, len) == 0) return true;
    return false;
}

/* §4.8 "attach a shadow root", given element, mode, clonable, serializable, delegatesFocus, slotAssignment and
   registry. Every one of its five refusals is a `NotSupportedError`, which a page's `catch (e) { e.name }`
   reads directly. Returns the shadow root's wrapper (OWNED) or JS_EXCEPTION. */
/* `registry` is §4.8's own parameter — "null or a CustomElementRegistry object" — which step 14 sets on the
   shadow root. JS_NULL means the spec's null (a shadow tree that looks a definition up in nothing), and it is
   NOT the same as "use the document's": attachShadow resolves that default at its step 1 before calling, and a
   default resolved HERE would give a declaratively-parsed root a registry §13.2.6.4.4 did not ask for. */
static JSValue sr_attach(JSContext *ctx, JSValueConst el_wrap, const char *mode, bool delegates_focus,
                         const char *slot_assignment, bool clonable, bool serializable,
                         JSValueConst registry)
{
    lxb_dom_node_t *n = node_of(el_wrap);
    lxb_dom_element_t *el;
    lxb_dom_shadow_root_t *sr;
    const lxb_char_t *local;
    size_t len = 0;
    JSValue current, wrap, slots;
    int state;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "attach a shadow root ran on something that is not an element");
    el = lxb_dom_interface_element(n);
    /* Step 1: "If element's namespace is not the HTML namespace, then throw a NotSupportedError." */
    if (lxb_dom_element_ns_id(el) != LXB_NS_HTML)
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "attachShadow on an element that is not in the HTML namespace");
    /* Step 2: a valid shadow host name. */
    local = lxb_dom_element_local_name(el, &len);
    DCHECK(local != NULL, "an element has no local name");
    if (!sr_valid_host_name((const char *)local, len))
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "attachShadow on an element whose local name is not a valid shadow host name");
    /* Step 3: a custom element definition whose `disable shadow` is true refuses. The second half of the
       condition — "or element's is value is non-null" — has nothing to test: an `is` value is set only for a
       CUSTOMIZED BUILT-IN, and §4.13.4 refuses to register one (ce_define_checks throws NotSupportedError for
       `extends`), so no element in this engine can carry one. It becomes a real read in the diff that makes
       customized built-ins registrable. */
    if (custom_elements_name_is_valid((const char *)local, len)) {
        /* §4.8 STEP 3.1 LOOKS THE DEFINITION UP AGAINST THE ELEMENT'S OWN REGISTRY — "given element's custom
           element registry, its namespace, its local name, and its is value" — not against the document's. A
           host inside a scoped tree must be refused by the definition ITS registry names, and answered by
           nothing when its registry is null. The by-name entry resolved the document's default and could not
           express either. */
        JSValue def = custom_elements_definition_lookup_for_element(ctx, el_wrap);
        bool disabled = JS_IsObject(def) && custom_elements_definition_flag(ctx, def, CE_DEF_DISABLE_SHADOW);

        JS_FreeValue(ctx, def);
        if (disabled)
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "this custom element's disabledFeatures contains \"shadow\"");
    }
    /* Step 4: an element that is ALREADY a shadow host. The whole branch turns on `declarative`, which HTML
       §13.2.6.4.4's `shadowrootmode` sets and nothing else does — so `attachShadow({mode:"open"})` on a host
       the PARSER gave an open declarative root EMPTIES that root and takes it over, while every other
       re-attach reaches step 4.2's throw. That difference is what the branch is for, and it is now reached:
       `declarative-shadow-dom-attachment.html` asserts both halves of it. */
    current = shadow_root_of_element_wrap(ctx, el_wrap);
    if (JS_IsObject(current)) {
        bool declarative = sr_flag(ctx, current, SR_DECLARATIVE);
        bool same_mode = shadow_root_is_open(node_of(current)) == (strcmp(mode, "open") == 0);

        if (!declarative || !same_mode) {                                            /* step 4.2 */
            JS_FreeValue(ctx, current);
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "attachShadow on an element that already has a shadow root");
        }
        {   /* step 4.3: remove all of currentShadowRoot's children, in tree order */
            lxb_dom_node_t *root = node_of(current), *c, *next;

            for (c = root->first_child; c; c = next) {
                next = c->next;
                dom_cow_remove_child(c);
            }
        }
        {   /* step 4.4 */
            JSValue rec = sr_slots(ctx, current);
            JS_SetPropertyStr(ctx, rec, SR_FIELD[SR_DECLARATIVE], JS_FALSE);
            JS_FreeValue(ctx, rec);
        }
        return current;                                                              /* step 4.5 */
    }
    JS_FreeValue(ctx, current);
    /* Step 5: "create a node that implements ShadowRoot, given element's node document". */
    sr = lxb_dom_shadow_root_interface_create(n->owner_document);
    CHECK(sr != NULL, "§4.8 step 5's ShadowRoot node could not be allocated — handing back a null the page "
                      "cannot tell from a root it never asked for is not an option");
    /* Steps 6-7: the host and the mode, in the node's own fields. Written before the node is wrapped, so no
       reader can ever see a shadow root whose host is null. */
    sr->host = el;
    sr->mode = (strcmp(mode, "open") == 0) ? LXB_DOM_SHADOW_ROOT_MODE_OPEN : LXB_DOM_SHADOW_ROOT_MODE_CLOSED;
    /* The running flow OWNS the node: it is destroyed with the flow's delta if the flow is discarded, exactly
       like an element the flow created and never inserted. */
    dom_cow_note_created(lxb_dom_interface_node(sr));
    wrap = node_wrap(ctx, lxb_dom_interface_node(sr));
    CHECK(JS_IsObject(wrap), "a ShadowRoot wrapper could not be allocated");
    slots = idl_slots_new(ctx);
    CHECK(JS_IsObject(slots), "§4.8's record could not be allocated");
    /* Steps 8-14, in the order the standard lists them. */
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_DELEGATES_FOCUS], JS_NewBool(ctx, delegates_focus));   /* 8 */
    state = custom_elements_state_of_element(ctx, el_wrap);                                          /* 9 */
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_AVAILABLE_TO_INTERNALS],
                      JS_NewBool(ctx, state == CE_STATE_PRECUSTOMIZED || state == CE_STATE_CUSTOM));
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_SLOT_ASSIGNMENT], JS_NewString(ctx, slot_assignment)); /* 10 */
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_DECLARATIVE], JS_FALSE);                               /* 11 */
    /* §4.8: `keep custom element registry null` is "initially false" — written here rather than left absent,
       because an absent slot and a false one read the same only until something asks the difference. */
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_KEEP_REGISTRY_NULL], JS_FALSE);
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_CLONABLE], JS_NewBool(ctx, clonable));                 /* 12 */
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_SERIALIZABLE], JS_NewBool(ctx, serializable));         /* 13 */
    JS_DefinePropertyValue(ctx, wrap, g_atom_slots, slots, SR_SLOT_FLAGS);
    /* STEP 14: "Set shadow's custom element registry to registry." It is the caller's answer, not this
       algorithm's — attachShadow resolved the default at its own step 1 and the declarative parser has its own
       — and it is written through the component that owns the association, because the once-only rule and the
       scoped-registry latch belong with the slot rather than with each writer. */
    custom_elements_node_associate_registry(ctx, wrap, registry);
    /* Step 15: "Set element's shadow root to shadow." */
    JS_DefinePropertyValue(ctx, (JSValue)el_wrap, g_atom_shadow, JS_DupValue(ctx, wrap), SR_SLOT_FLAGS);
    return wrap;
}

JSValue shadow_root_attach(JSContext *ctx, JSValueConst el_wrap, const char *mode, bool delegates_focus,
                           const char *slot_assignment, bool clonable, bool serializable,
                           JSValueConst registry)
{
    return sr_attach(ctx, el_wrap, mode, delegates_focus, slot_assignment, clonable, serializable, registry);
}

/* DOM §4.4 "clone a node" STEP 6, its own steps 6.1-6.7. The standard runs it AFTER step 5 has cloned the light
   children, and it is NOT conditioned on `subtree`: `host.cloneNode(false)` still gets the shadow tree, cloned
   deeply, because step 6.8 passes subtree TRUE whatever the caller asked for. Every field step 6 PASSES or SETS
   is read off the original's record — the one thing 6.5 hardcodes is `clonable` itself, which it passes as
   true, and that is the same value the original has because it is the condition for being here at all. What
   the new root does NOT take from the original is `available to element internals`: that one is §4.8 step 9's,
   computed from the COPY's own custom element state, which is what the standard says it is. */
JSValue shadow_root_clone_onto(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *copy)
{
    JSValueConst el_wrap;
    JSValue src, copy_wrap, current, sr, rec, src_reg;
    bool declarative, keep_null;

    DCHECK(g_ready, "§4.4 step 6 ran before shadow_root_init");
    DCHECK(node != NULL && copy != NULL, "§4.4 step 6 was asked about no node");
    /* Step 6's three conditions. The first is the node's type; the second and third are the association and the
       record, and a host that has NEITHER answers without allocating — an element with no wrapper cannot have
       had `attachShadow` called on it. */
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_NULL;
    el_wrap = node_wrap_peek(node);
    if (!JS_IsObject(el_wrap))
        return JS_NULL;
    src = shadow_root_of_element_wrap(ctx, el_wrap);
    if (!JS_IsObject(src)) { JS_FreeValue(ctx, src); return JS_NULL; }
    if (!sr_flag(ctx, src, SR_CLONABLE)) { JS_FreeValue(ctx, src); return JS_NULL; }
    /* The copy is about to hold an association, which lives on ITS wrapper, so this is where one is minted. */
    copy_wrap = node_wrap(ctx, copy);
    CHECK(JS_IsObject(copy_wrap), "§4.4 step 6: the clone's wrapper could not be allocated");
    /* Step 6.1: "Assert: copy is not a shadow host." It is a `clone_interface` node made moments ago and the
       association is a slot on a wrapper minted moments ago, so nothing can have attached one. */
    current = shadow_root_of_element_wrap(ctx, copy_wrap);
    DCHECK(!JS_IsObject(current), "§4.4 step 6.1: the copy is already a shadow host, so the attach below would "
                                  "reach §4.8 step 4 and either throw or take over a root the clone invented");
    JS_FreeValue(ctx, current);
    /* STEPS 6.2-6.4: `shadowRootRegistry` is the ORIGINAL root's registry, which is now a real read — a host
       inside a scoped tree clones into a copy that looks its definitions up in the same scoped registry, which
       is the whole reason a registry is per node rather than per document.
       Step 6.5: attach a shadow root with the ORIGINAL's mode, serializable, delegates focus and slot
       assignment, and `clonable` true. */
    src_reg = custom_elements_node_registry(ctx, src);
    sr = sr_attach(ctx, copy_wrap, shadow_root_is_open(node_of(src)) ? "open" : "closed",
                   sr_flag(ctx, src, SR_DELEGATES_FOCUS),
                   shadow_root_slot_assignment_is_manual(ctx, node_of(src)) ? "manual" : "named",
                   true, sr_flag(ctx, src, SR_SERIALIZABLE), src_reg);
    JS_FreeValue(ctx, src_reg);
    /* Step 6.6: "Set copy's shadow root's declarative to node's shadow root's declarative." NOT
       shadow_root_mark_declarative, which is HTML §13.2.6.4.4's pair of writes: that one also sets `available
       to element internals`, and step 6 does not — the clone's is whatever §4.8 step 9 just computed from the
       COPY's own custom element state, which is the state the standard says it is. */
    declarative = sr_flag(ctx, src, SR_DECLARATIVE);
    /* STEP 6.7's flag is read HERE, beside step 6.6's, because both are read off the ORIGINAL and the original
       is released on the next line. It rides with the registry it guards: a declaratively-parsed root that
       resolves in nothing clones into one that still resolves in nothing, rather than into one the next
       adoption hands the document's registry. */
    keep_null = sr_flag(ctx, src, SR_KEEP_REGISTRY_NULL);
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, copy_wrap);
    if (JS_IsException(sr))
        return sr;
    rec = sr_slots(ctx, sr);
    DCHECK(JS_IsObject(rec), "§4.4 step 6.6: the shadow root attach a shadow root just made has no §4.8 record");
    JS_SetPropertyStr(ctx, rec, SR_FIELD[SR_DECLARATIVE], JS_NewBool(ctx, declarative));
    JS_SetPropertyStr(ctx, rec, SR_FIELD[SR_KEEP_REGISTRY_NULL], JS_NewBool(ctx, keep_null));   /* step 6.7 */
    JS_FreeValue(ctx, rec);
    return sr;
}

/* §4.8's `keep custom element registry null`. HTML §13.2.6.4.4 is the only writer — a
   `<template shadowrootcustomelementregistry>` — and DOM §4.5's adopt is the only reader, which is why both
   halves are exported rather than kept private: without the flag that attribute is undone by the first
   adoption, since adopt gives a shadow root with a NULL registry the new document's unless this says not to. */
void shadow_root_set_keep_registry_null(JSContext *ctx, JSValueConst sr_wrap)
{
    JSValue rec = sr_slots(ctx, sr_wrap);

    DCHECK(JS_IsObject(rec), "§4.8's keep-custom-element-registry-null was set on something with no §4.8 record");
    JS_SetPropertyStr(ctx, rec, SR_FIELD[SR_KEEP_REGISTRY_NULL], JS_TRUE);
    JS_FreeValue(ctx, rec);
}

bool shadow_root_keep_registry_null(JSContext *ctx, JSValueConst sr_wrap)
{
    return sr_flag(ctx, sr_wrap, SR_KEEP_REGISTRY_NULL);
}

void shadow_root_mark_declarative(JSContext *ctx, JSValueConst sr_wrap)
{
    JSValue rec;

    DCHECK(shadow_root_is(node_of(sr_wrap)),
           "HTML §13.2.6.4.4 marked something that is not a shadow root as declarative");
    rec = sr_slots(ctx, sr_wrap);
    DCHECK(JS_IsObject(rec), "a shadow root has no §4.8 record — the record is built by attach a shadow root, "
                             "which is the only thing that makes one of these");
    /* "Set shadow's declarative to true", and "set shadow's available to element internals to true" — the
       second unconditionally, where step 9 set it only for a custom or precustomized host: a declarative root
       is available to `ElementInternals.shadowRoot` whatever the host's custom element state is, because the
       author wrote it in the markup the element upgrades out of. */
    JS_SetPropertyStr(ctx, rec, SR_FIELD[SR_DECLARATIVE], JS_TRUE);
    JS_SetPropertyStr(ctx, rec, SR_FIELD[SR_AVAILABLE_TO_INTERNALS], JS_TRUE);
    JS_FreeValue(ctx, rec);
}

/* §4.9 `attachShadow(init)`. `init` is a dictionary, so the READ of each member is the page's code and the
   declaration is what performs it — by the time this body runs every member is a real value. */
static const IdlArgType ATTACH_ARGS[1] = { IDL_DICT };
static const char *const SR_MODE_VALUES[] = { "open", "closed", NULL };
static const char *const SR_SLOT_VALUES[] = { "manual", "named", NULL };
/* ShadowRootInit. Web IDL §3.2.18 reads a dictionary's members LEXICOGRAPHICALLY, not in declaration order —
   which for this dictionary is a different order in every position, and is observable the moment a page passes
   an object whose members are getters or a Proxy. `mode` being required does not move it to the front. */
static const IdlDictMember SHADOW_ROOT_INIT[] = {
    { "clonable",       IDL_BOOLEAN, false, NULL,           0 },
    { "delegatesFocus", IDL_BOOLEAN, false, NULL,           0 },
    { "mode",           IDL_ENUM,    true,  SR_MODE_VALUES, 0 },
    { "serializable",   IDL_BOOLEAN, false, NULL,           0 },
    { "slotAssignment", IDL_ENUM,    false, SR_SLOT_VALUES, 0 },
};

static JSValue js_el_attach_shadow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue mode_v, slot_v, reg_v, registry, result;
    const char *mode, *slot_assignment;

    (void)magic;
    if (!node_of(this_val) || node_of(this_val)->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_ThrowTypeError(ctx, "attachShadow called on something that is not an element");
    /* STEPS 1-3, THE REGISTRY CHECK. Step 1's default is this document's registry; step 2 replaces it with
       `init["customElementRegistry"]` when the page supplied one; step 3 throws when that one is neither
       SCOPED nor this document's own — which is the whole point of the member: a page may hand a shadow tree a
       scoped registry, or the very registry the document already uses, and nothing else.
       THE BRAND TEST IS STEP 2's, not an extra: the declaration converts the member to an object and an object
       that is not a CustomElementRegistry must not be associated with the root as though it were. */
    reg_v = idl_dict_get(ctx, init, "customElementRegistry");
    registry = custom_elements_document_registry(ctx);                                   /* step 1 */
    if (JS_IsObject(reg_v)) {
        if (!custom_elements_is_registry(reg_v)) {
            JS_FreeValue(ctx, registry);
            JS_FreeValue(ctx, reg_v);
            return JS_ThrowTypeError(ctx, "ShadowRootInit's customElementRegistry is not a "
                                          "CustomElementRegistry");
        }
        JS_FreeValue(ctx, registry);
        registry = JS_DupValue(ctx, reg_v);                                              /* step 2 */
    }
    JS_FreeValue(ctx, reg_v);
    if (JS_IsObject(registry) && !custom_elements_registry_is_scoped(ctx, registry)) {   /* step 3 */
        JSValue doc_reg = custom_elements_document_registry(ctx);
        bool same = JS_VALUE_GET_PTR(doc_reg) == JS_VALUE_GET_PTR(registry);

        JS_FreeValue(ctx, doc_reg);
        if (!same) {
            JS_FreeValue(ctx, registry);
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "attachShadow was given a custom element registry that is neither "
                                        "scoped nor this document's");
        }
    }
    mode_v = idl_dict_get(ctx, init, "mode");
    DCHECK(JS_IsString(mode_v), "ShadowRootInit's `mode` is required and the declaration converts it, so a "
                                "body reaching here without a string means the conversion was skipped");
    slot_v = idl_dict_get(ctx, init, "slotAssignment");
    mode = JS_ToCString(ctx, mode_v);
    /* `slotAssignment` defaults to "named" — a default the DECLARATION does not apply (an absent member is
       absent), so the member's own IDL default is stated here, where the IDL states it. */
    slot_assignment = JS_IsString(slot_v) ? JS_ToCString(ctx, slot_v) : NULL;
    result = sr_attach(ctx, this_val, mode ? mode : "open", idl_dict_bool(ctx, init, "delegatesFocus"),
                       slot_assignment ? slot_assignment : "named",
                       idl_dict_bool(ctx, init, "clonable"), idl_dict_bool(ctx, init, "serializable"),
                       registry);
    JS_FreeValue(ctx, registry);
    if (mode) JS_FreeCString(ctx, mode);
    if (slot_assignment) JS_FreeCString(ctx, slot_assignment);
    JS_FreeValue(ctx, mode_v);
    JS_FreeValue(ctx, slot_v);
    return result;
}

/* §4.9's `shadowRoot` getter: "Let shadow be this's shadow root. If shadow is null or its mode is 'closed',
   then return null. Return shadow." A CLOSED root is unreachable from script — that is the whole of what the
   mode means, and the reason the association is a slot rather than a property. */
static JSValue js_el_shadow_root(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue sr;

    (void)magic;
    if (!node_of(this_val) || node_of(this_val)->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_ThrowTypeError(ctx, "the shadowRoot getter ran on something that is not an element");
    sr = shadow_root_of_element_wrap(ctx, this_val);
    if (!JS_IsObject(sr)) return JS_NULL;
    if (!shadow_root_is_open(node_of(sr))) { JS_FreeValue(ctx, sr); return JS_NULL; }
    return sr;
}

/* ---- declaration and installation ----------------------------------------------------------------------------- */

void shadow_root_init(JSContext *ctx)
{
    JSClassDef d = { "ShadowRoot" };

    DCHECK(!g_ready, "shadow_root_init ran twice — the interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_sr_class);
    JS_NewClass(JS_GetRuntime(ctx), g_sr_class, &d);
    node_claim_type(LXB_DOM_NODE_TYPE_SHADOW_ROOT, g_sr_class);
    g_slots_key = JS_NewSymbol(ctx, "ShadowRoot §4.8 record", true);
    CHECK(JS_IsSymbol(g_slots_key), "§4.8's record key could not be minted");
    g_atom_slots = JS_ValueToAtom(ctx, g_slots_key);
    g_shadow_key = JS_NewSymbol(ctx, "Element shadow root", true);
    CHECK(JS_IsSymbol(g_shadow_key), "§4.9's shadow-root association key could not be minted");
    g_atom_shadow = JS_ValueToAtom(ctx, g_shadow_key);
    g_id_attach = idl_method_id_dict(ctx, ATTACH_ARGS, 1, SHADOW_ROOT_INIT,
                                     (int)(sizeof(SHADOW_ROOT_INIT) / sizeof(SHADOW_ROOT_INIT[0])),
                                     js_el_attach_shadow, 0);
    /* HTML §8.5's THREE MARKUP MEMBERS ON THIS INTERFACE, and each is the SAME algorithm Element's is — which
       is why not one of them is implemented here. §8.5.4's `innerHTML` getter is §13.3's serializer with the
       shadow options false and « » (the component that owns §13.3); its setter and §8.5.2's `setHTMLUnsafe`
       are §13.4's fragment parse, whose §13.4 step 2 says the context element is "target's HOST" when the
       target is not an element — one line of difference, expressed as a magic on element.c's one parse machine
       rather than as a second parse that can drift from it.
       `getHTML` is that component's own declaration, installed on both prototypes.
       `setHTML` — the SAFE member — is the SAME machine with §8.6.4's `safe` true: what it filters with is
       §8.6's sanitizer, and its own declaration is element.c's for the reason setHTMLUnsafe's is. */
    g_id_inner_get = idl_getter_id_step(ctx, fragment_serializer_decl(), FRAGMENT_SERIALIZE_CHILDREN);
    g_id_inner_set = idl_setter_id_step(ctx, IDL_DOMSTRING, true, element_set_html_decl(),
                                        SHADOW_ROOT_SET_INNER_HTML);
    g_id_set_html_unsafe = element_declare_set_html_unsafe(ctx, SHADOW_ROOT_SET_HTML_UNSAFE);
    g_id_set_html = element_declare_set_html(ctx, SHADOW_ROOT_SET_HTML);
    g_ready = 1;
    realm_declare_intrinsic(shadow_root_install_proto);
}

void shadow_root_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    prev = JS_GetClassProto(ctx, g_sr_class);
    DCHECK(JS_IsNull(prev), "shadow_root_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* §4.8: `interface ShadowRoot : DocumentFragment`, so the chain is the fragment's — which is what gives a
       shadow root `querySelector`, `append` and `getElementById` without a third copy of either mixin. */
    base = document_fragment_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "ShadowRoot.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ShadowRoot");
    idl_install_accessor(ctx, proto, "mode", js_sr_get, SR_MODE, -1);
    idl_install_accessor(ctx, proto, "delegatesFocus", js_sr_get, SR_DELEGATES_FOCUS, -1);
    idl_install_accessor(ctx, proto, "slotAssignment", js_sr_get, SR_SLOT_ASSIGNMENT, -1);
    idl_install_accessor(ctx, proto, "clonable", js_sr_get, SR_CLONABLE, -1);
    idl_install_accessor(ctx, proto, "serializable", js_sr_get, SR_SERIALIZABLE, -1);
    idl_install_accessor(ctx, proto, "host", js_sr_get, SR_HOST, -1);
    /* HTML §8.5's partial interface. `serializable` above is no longer a flag with no reader: `getHTML`'s
       serializableShadowRoots argument is what reads it, and §13.3 step 4.2 is where. */
    idl_install_accessor_step(ctx, proto, "innerHTML", g_id_inner_get, g_id_inner_set);
    idl_install_method(ctx, proto, "setHTML", 1, g_id_set_html);
    idl_install_method(ctx, proto, "setHTMLUnsafe", 1, g_id_set_html_unsafe);
    fragment_serializer_install_get_html(ctx, proto);
    /* HTML §6.6.6's `DocumentOrShadowRoot` addition. It is the same one member Document carries, over the same
       focused area, and it is the RECEIVER that decides the answer: §4.8's retargeting against `this` is what
       turns a focused node inside this tree into the node an observer of this tree may see. */
    focus_install_shadow_root_members(ctx, proto);
    /* §4.8's ONE event handler IDL attribute. It is declared on ShadowRoot itself and not through
       GlobalEventHandlers, which is why it needs its own bit rather than riding EH_GLOBAL's mask. */
    event_target_install_handlers(ctx, proto, EH_SHADOW_ROOT);
    JS_SetClassProto(ctx, g_sr_class, proto);
}

void shadow_root_install_element_members(JSContext *ctx, JSValueConst element_proto)
{
    DCHECK(g_ready, "§4.9's shadow members were installed before shadow_root_init ran");
    idl_install_method(ctx, element_proto, "attachShadow", 1, g_id_attach);
    idl_install_accessor(ctx, element_proto, "shadowRoot", js_el_shadow_root, 0, -1);
}

void shadow_root_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_sr_class);

    DCHECK(g_ready, "the ShadowRoot interface object was installed before shadow_root_init ran");
    DCHECK(!JS_IsNull(proto), "ShadowRoot's interface object was installed in a realm that never ran its "
                              "prototype install");
    /* §4.8 declares no constructor: the interface object exists to be what `instanceof` names. */
    JS_SetPropertyStr(ctx, (JSValue)global, "ShadowRoot", idl_interface_object(ctx, "ShadowRoot", proto));
    JS_FreeValue(ctx, proto);
}

void shadow_root_free(JSContext *ctx)
{
    if (!g_ready) return;
    /* The slot keys are the AGENT's — a Symbol nobody frees is a live GC object the runtime's own walk counts
       as a leak. The prototypes are the REALMS', released with their contexts. */
    JS_FreeAtom(ctx, g_atom_slots);
    JS_FreeAtom(ctx, g_atom_shadow);
    g_atom_slots = g_atom_shadow = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_slots_key);
    JS_FreeValue(ctx, g_shadow_key);
    g_slots_key = g_shadow_key = JS_UNDEFINED;
    g_id_attach = g_id_inner_get = g_id_inner_set = g_id_set_html_unsafe = g_id_set_html = -1;
    g_ready = 0;
}
