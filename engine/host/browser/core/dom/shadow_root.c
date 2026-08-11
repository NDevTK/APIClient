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
 * WHAT IS HONESTLY ABSENT, BY NAME — see SPEC_STEPS.md §17.6. `ShadowRootInit`'s `customElementRegistry`
 * member, `declarative` shadow roots (the parser's `shadowrootmode`), `clonable`'s effect in `cloneNode`,
 * `serializable`'s effect in `getHTML`, `delegatesFocus`'s effect on focus, and HTML's
 * `DocumentOrShadowRoot`/`ShadowRoot` additions (`innerHTML`, `activeElement`, `styleSheets`). */
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
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "solver/dom_cow.h"

static JSClassID g_sr_class;
static int       g_ready;
static int       g_id_attach = -1;

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
enum { SR_MODE = 0, SR_DELEGATES_FOCUS, SR_SLOT_ASSIGNMENT, SR_CLONABLE, SR_SERIALIZABLE, SR_HOST,
       SR_AVAILABLE_TO_INTERNALS, SR_DECLARATIVE };
static const char *const SR_FIELD[] = {
    "mode", "delegatesFocus", "slotAssignment", "clonable", "serializable", "host",
    "availableToElementInternals", "declarative"
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
static JSValue sr_attach(JSContext *ctx, JSValueConst el_wrap, const char *mode, bool delegates_focus,
                         const char *slot_assignment, bool clonable, bool serializable)
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
        JSValue def = custom_elements_definition_for_name(ctx, (const char *)local, len);
        bool disabled = JS_IsObject(def) && custom_elements_definition_flag(ctx, def, CE_DEF_DISABLE_SHADOW);

        JS_FreeValue(ctx, def);
        if (disabled)
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "this custom element's disabledFeatures contains \"shadow\"");
    }
    /* Step 4: an element that is ALREADY a shadow host. The whole branch turns on `declarative`, which only the
       HTML parser's `shadowrootmode` can set and which this engine therefore never sets — so every re-attach
       reaches the throw. It is written as the standard writes it rather than collapsed to that throw, because
       the day the parser builds a declarative root the difference is the whole of step 4. */
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
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_CLONABLE], JS_NewBool(ctx, clonable));                 /* 12 */
    JS_SetPropertyStr(ctx, slots, SR_FIELD[SR_SERIALIZABLE], JS_NewBool(ctx, serializable));         /* 13 */
    /* Step 14's `custom element registry` is the document's, which is what looking up a definition already
       reads — see SPEC_STEPS.md §17.6 for why the init member that could override it is absent. */
    JS_DefinePropertyValue(ctx, wrap, g_atom_slots, slots, SR_SLOT_FLAGS);
    /* Step 15: "Set element's shadow root to shadow." */
    JS_DefinePropertyValue(ctx, (JSValue)el_wrap, g_atom_shadow, JS_DupValue(ctx, wrap), SR_SLOT_FLAGS);
    return wrap;
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
    JSValue mode_v, slot_v, result;
    const char *mode, *slot_assignment;

    (void)magic;
    if (!node_of(this_val) || node_of(this_val)->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_ThrowTypeError(ctx, "attachShadow called on something that is not an element");
    /* Steps 1-3 of `attachShadow` are the registry check, and this engine has exactly one registry, which IS
       the node document's — so `registry` is that one, "is scoped" is false for it, and the comparison in step
       3 cannot fail. The member that could make it fail is absent by name (SPEC_STEPS.md §17.6). */
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
                       idl_dict_bool(ctx, init, "clonable"), idl_dict_bool(ctx, init, "serializable"));
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
    g_id_attach = -1;
    g_ready = 0;
}
