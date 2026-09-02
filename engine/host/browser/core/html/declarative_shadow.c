/* DECLARATIVE SHADOW ROOTS — HTML §4.12.3's six `<template>` content attributes, the two reflections that are
 * not plain mirrors, and §13.2.6.4.4's "A start tag whose tag name is template" in the "in head" insertion mode.
 *
 * WHAT WAS MISSING. `shadowrootmode` was not read anywhere in this engine, so `<template shadowrootmode=open>`
 * parsed to an ordinary `<template>` element and stayed one: the host got no shadow root, the markup the author
 * wrote for the shadow tree stayed inside a fragment nothing renders, and every slot in it held nothing. It is
 * also the ONLY writer of a shadow root's `declarative`, which is what makes DOM §4.9 "attach a shadow root" step 4
 * — the re-attach branch shadow_root.c writes out in full — reachable at all. A page that ships its components
 * as declarative shadow DOM (which is what server-rendered Web Components are) had NO shadow trees, so every
 * `host.shadowRoot.querySelector(...)` in its own script threw on the null.
 *
 * WHAT THE STANDARD SAYS, AND WHERE THIS PUTS IT. The step runs inside tree construction, on the START TAG: the
 * `<template>` is created and pushed onto the stack of open elements but NEVER INSERTED (HTML §13.2.6.4.4's
 * "insert a foreign element for templateStartTag, with HTML namespace and true"), a shadow root is attached to
 * the adjusted current node, and the template's TEMPLATE CONTENTS are then set to that shadow root — which is
 * why the markup that follows lands in the shadow tree: HTML §13.2.6.1 Creating and inserting nodes' "the
 * appropriate place for inserting a node" inside a template IS its template contents.
 * Lexbor's tree builder has no such step: it inserts the template and fills its own content fragment. This
 * component therefore runs the step at the PARSE BOUNDARY, over the tree the parse produced, and joins the two
 * ends the standard never separates — the contents lexbor collected are MOVED into the shadow root and the
 * template element the standard never inserted is discarded. See declarative_shadow.h for why the boundary is
 * where a wrapper can first exist, and why nothing can observe the difference.
 *
 * THE THREE CONDITIONS ARE ALL THREE. The step is taken only when the mode is not the None state, the parser's
 * "allow declarative shadow roots" is true, and the adjusted current node is not the topmost element in the
 * stack of open elements. The third is what makes `div.setHTMLUnsafe("<template shadowrootmode=open>")` leave a
 * template rather than attaching a shadow root to the fragment parsing algorithm's own root element, and after
 * a parse it is answerable exactly: the topmost element is the document element, or the fragment's root.
 *
 * EVERY REFUSAL IS CAUGHT. "Attach a shadow root" throws `NotSupportedError` five ways — a `<progress>` is not
 * a valid shadow host name, a custom element may disable shadows — and the standard CATCHES it and leaves the
 * template in the tree. A parse never throws at the page; `declarative-shadow-dom-basic.html` asserts exactly
 * that with a `window.onerror` listener. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/html/declarative_shadow.h"
#include "core/html/custom_elements.h"
#include "core/idl_args.h"
#include "solver/dom_cow.h"

static int g_ready;
static int g_id_enum_set[2] = { -1, -1 };   /* one per enumerated reflection — see the table below */

/* ---- HTML §4.12.3's content attributes ------------------------------------------------------------------- */

/* §2.3.3 Keywords and enumerated attributes: a keyword is matched ASCII case-insensitively. `strcasecmp`
   is the LOCALE's answer, and the standard's is ASCII's — a Turkish locale folds `I` to `ı` and would
   fail to recognise `shadowrootmode="OPEN"`. One comparison, written once, over a length the caller already has. */
static bool ascii_ieq(const char *a, size_t an, const char *b)
{
    size_t i;

    if (strlen(b) != an) return false;
    for (i = 0; i < an; i++) {
        char x = a[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (x != b[i]) return false;
    }
    return true;
}

/* A content attribute's value, or NULL when the element does not have it — the read both the reflections and
   the parser step are built on. Borrowed from lexbor's own storage. */
static const char *attr_of(const lxb_dom_element_t *el, const char *name, size_t *plen)
{
    const lxb_char_t *v;
    size_t vl = 0;

    v = lxb_dom_element_get_attribute((lxb_dom_element_t *)el, (const lxb_char_t *)name, strlen(name), &vl);
    *plen = vl;
    return (const char *)v;
}

/* §2.3.2 a BOOLEAN attribute is its PRESENCE: `shadowrootclonable=""` and `shadowrootclonable="false"` both
   mean true, which is why this cannot be a value test. */
static bool bool_attr(const lxb_dom_element_t *el, const char *name)
{
    size_t vl = 0;

    return attr_of(el, name, &vl) != NULL;
}

/* THE TWO ENUMERATED ATTRIBUTES, as the table their reflections and the parser step both read. `states` lists
   the keywords in canonical form; `missing_invalid` is BOTH the missing value default and the invalid value
   default, which for these two attributes are the same state — index -1 for `shadowrootmode`'s None state,
   which has no keyword, and 0 for `shadowrootslotassignment`'s Named. */
typedef struct { const char *attr; const char *const *states; int missing_invalid; } DsdEnum;
static const char *const DSD_MODE_STATES[] = { "open", "closed", NULL };
static const char *const DSD_SLOT_STATES[] = { "named", "manual", NULL };
enum { DSD_MODE = 0, DSD_SLOT_ASSIGNMENT };
static const DsdEnum DSD_ENUM[] = {
    { "shadowrootmode",           DSD_MODE_STATES, -1 },
    { "shadowrootslotassignment", DSD_SLOT_STATES,  0 },
};

/* WHICH STATE this element's attribute is in, as an index into `states` — or the attribute's missing/invalid
   value default, which is the whole of what "invalid value default" means. */
static int enum_state(const lxb_dom_element_t *el, int which)
{
    const DsdEnum *e = &DSD_ENUM[which];
    size_t vl = 0;
    const char *v = attr_of(el, e->attr, &vl);
    int i;

    if (!v) return e->missing_invalid;
    for (i = 0; e->states[i]; i++)
        if (ascii_ieq(v, vl, e->states[i])) return i;
    return e->missing_invalid;
}

/* §2.6.1's getter for a reflected enumerated attribute LIMITED TO ONLY KNOWN VALUES: return the CANONICAL
   keyword of the state the attribute's value corresponds to, or "" when that state has no keyword. So
   `shadowRootMode` answers "closed" for `shadowrootmode="CLOSED"` and "" for `shadowrootmode="nonsense"`,
   while `shadowRootSlotAssignment` answers "named" for both an absent attribute and a nonsense one — the two
   attributes differ only in which state their invalid value default is. */
static JSValue js_tpl_enum_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    int state;

    DCHECK(magic >= 0 && magic < (int)(sizeof(DSD_ENUM) / sizeof(DSD_ENUM[0])),
           "a §4.12.3 enumerated reflection was installed with a magic the table does not name");
    /* WEB IDL §3.7.6 Attributes' BRAND CHECK, and it is a THROW rather than an assert: a page reaches an
       accessor off
       the prototype with `.call` on anything it likes, so "the receiver is a <template>" is the PAGE's input
       and not this engine's invariant. */
    if (!el || !lxb_html_tree_node_is(lxb_dom_interface_node(el), LXB_TAG_TEMPLATE))
        return JS_ThrowTypeError(ctx, "an HTMLTemplateElement member was read on something that is not a "
                                      "<template> element");
    state = enum_state(el, magic);
    if (state < 0) return JS_NewStringLen(ctx, "", 0);
    return JS_NewString(ctx, DSD_ENUM[magic].states[state]);
}

/* §2.6.1's setter for the same: "set the content attribute to the given value", verbatim. The mapping to a
   state happens on the way OUT, which is why `t.shadowRootMode = "blah"` leaves `shadowrootmode="blah"` on the
   element and reads back as "". */
static JSValue js_tpl_enum_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    const char *s;

    DCHECK(magic >= 0 && magic < (int)(sizeof(DSD_ENUM) / sizeof(DSD_ENUM[0])),
           "a §4.12.3 enumerated reflection was installed with a magic the table does not name");
    if (!el || !lxb_html_tree_node_is(lxb_dom_interface_node(el), LXB_TAG_TEMPLATE))
        return JS_ThrowTypeError(ctx, "an HTMLTemplateElement member was written on something that is not a "
                                      "<template> element");
    /* A real string by now: the declaration's IDL_DOMSTRING ran the page's `toString` before this body. */
    DCHECK(JS_IsString(val), "a reflected DOMString reached its setter unconverted — the declaration is what "
                             "converts it, and running the page's toString from here is the "
                             "drive-to-completion the flow machinery exists to avoid");
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, DSD_ENUM[magic].attr, s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- HTML §13.2.6.4.4's template start tag ---------------------------------------------------------------- */

/* THE TREES STILL TO WALK. A `<template>`'s contents and a shadow root are each a tree hanging off a node
   rather than a child of it, so the walk cannot follow child links alone — and BOTH can hold another
   `<template shadowrootmode>`. The list is explicit storage and not C recursion for the reason every other tree
   walk in this engine is: the nesting is the PAGE's, and a component inside a component inside a component is
   as deep as the author wrote it. */
typedef struct { lxb_dom_node_t **v; int n, cap; } DsdTrees;

static void dsd_push(DsdTrees *t, lxb_dom_node_t *tree)
{
    if (!tree) return;
    if (t->n == t->cap) {
        int want = t->cap ? t->cap * 2 : 8;
        lxb_dom_node_t **v = realloc(t->v, sizeof(*v) * (size_t)want);
        CHECK(v != NULL, "the declarative shadow root walk could not grow its list of trees — dropping one "
                         "leaves a shadow tree the parser wrote with its own <template> unconverted");
        t->v = v;
        t->cap = want;
    }
    t->v[t->n++] = tree;
}

/* A `<template>`'s template contents. Every template element has one — §4.12.3 establishes it when the element
   is created, and lexbor's template interface constructor is where that happens here. */
static lxb_dom_node_t *template_contents(lxb_dom_node_t *n)
{
    lxb_html_template_element_t *t = lxb_html_interface_template(n);

    DCHECK(t->content != NULL, "a <template> element has no template contents — §4.12.3 establishes them when "
                               "the element is created, so an element without them was made some other way");
    return &t->content->node;
}

void declarative_shadow_parsed(JSContext *ctx, lxb_dom_node_t *tree, const lxb_dom_node_t *topmost, bool allow)
{
    DsdTrees todo = { NULL, 0, 0 };

    DCHECK(g_ready, "the parser's declarative shadow roots ran before declarative_shadow_init");
    /* The step's second condition. A document whose "allow declarative shadow roots" is false — an XHR
       `responseXML`, a `DOMParser` result, a `createHTMLDocument` — reaches "insert an HTML element for the
       token", which is what the parse already did, so there is nothing at all to do. */
    if (!allow || !tree) return;
    dsd_push(&todo, tree);
    while (todo.n) {
        lxb_dom_node_t *root = todo.v[--todo.n], *n, *next;

        for (n = node_next_in(root, root); n; n = next) {
            lxb_dom_node_t *contents, *host, *shadow, *child, *cnext;
            JSValue host_wrap, current, sr, registry;
            int mode;

            next = node_next_in(n, root);
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE))
                continue;
            contents = template_contents(n);
            mode = enum_state(lxb_dom_interface_element(n), DSD_MODE);
            host = n->parent;
            /* The step's first and third conditions, plus the one a finished parse states differently. A
               template whose parent is NOT AN ELEMENT is one lexbor put in another template's contents, and
               the standard's adjusted current node there is that OUTER TEMPLATE ELEMENT — whose local name is
               not a valid shadow host name, so "attach a shadow root" throws at step 2 and the catch leaves
               the template exactly where this branch does. Answered here rather than by attaching to a
               fragment, because the two answers are the same one and only this one is expressible.
               Any of the three and the template stays a template — its contents stay its own, and they are
               walked, because a declarative shadow root nested inside them has an ELEMENT for its host. */
            if (mode < 0 || !host || host->type != LXB_DOM_NODE_TYPE_ELEMENT || host == topmost) {
                dsd_push(&todo, contents);
                continue;
            }
            host_wrap = node_wrap(ctx, host);
            CHECK(JS_IsObject(host_wrap), "a declarative shadow host could not be wrapped");
            /* "If declarativeShadowHostElement is a shadow host, then insert an element at the adjusted
               insertion location with template." The SECOND `<template shadowrootmode>` under one host is left
               in the DOM as an ordinary template — it must NOT reach "attach a shadow root", whose step 4
               would empty the first root and take it over. */
            current = shadow_root_of_element_wrap(ctx, host_wrap);
            if (JS_IsObject(current)) {
                JS_FreeValue(ctx, current);
                JS_FreeValue(ctx, host_wrap);
                dsd_push(&todo, contents);
                continue;
            }
            JS_FreeValue(ctx, current);
            /* "Let registry be NULL if templateStartTag has a shadowrootcustomelementregistry attribute;
               otherwise declarativeShadowHostElement's node document's custom element registry."
               THE ATTRIBUTE'S SENSE IS THE OPPOSITE OF ITS NAME, which is why it is written out rather than
               read as a flag: `shadowrootcustomelementregistry` does not NAME a registry and does not opt the
               tree INTO one — it says the root's registry is null, so the tree resolves nothing until
               something associates one with it. Read the other way round, a declarative shadow root carrying
               the attribute would have been given the document's registry, which is the one answer the
               attribute exists to prevent.
               "Attach a shadow root with declarativeShadowHostElement, mode, clonable, serializable,
               delegatesFocus, slotAssignment, and registry." */
            registry = bool_attr(lxb_dom_interface_element(n), "shadowrootcustomelementregistry")
                           ? JS_NULL : custom_elements_document_registry(ctx);
            sr = shadow_root_attach(ctx, host_wrap, mode == 0 ? "open" : "closed",
                                    bool_attr(lxb_dom_interface_element(n), "shadowrootdelegatesfocus"),
                                    enum_state(lxb_dom_interface_element(n), DSD_SLOT_ASSIGNMENT) == 1
                                        ? "manual" : "named",
                                    bool_attr(lxb_dom_interface_element(n), "shadowrootclonable"),
                                    bool_attr(lxb_dom_interface_element(n), "shadowrootserializable"),
                                    registry);
            JS_FreeValue(ctx, registry);
            JS_FreeValue(ctx, host_wrap);
            /* "If templateStartTag has a shadowrootcustomelementregistry attribute, then set shadow's keep
               custom element registry null to true." It is the SECOND half of that attribute and neither half
               works alone: the null registry above says what the root resolves in NOW, and this says the first
               adoption must not quietly replace it with the new document's. */
            if (!JS_IsException(sr) && JS_IsObject(sr)
                && bool_attr(lxb_dom_interface_element(n), "shadowrootcustomelementregistry"))
                shadow_root_set_keep_registry_null(ctx, sr);
            if (JS_IsException(sr)) {
                /* "If an exception is thrown, then catch it and: insert an element at the adjusted insertion
                   location with template; the user agent MAY report an error to the developer console;
                   return." A `<progress>` is not a valid shadow host name and a custom element may disable
                   shadows — and a parse that threw at the page would be a parse error the page can see, which
                   the standard does not have. */
                JS_FreeValue(ctx, JS_GetException(ctx));
                dsd_push(&todo, contents);
                continue;
            }
            shadow = node_of(sr);
            DCHECK(shadow_root_is(shadow), "attach a shadow root answered something that is not a shadow root");
            /* "Set template's template contents to shadow." Lexbor's tree builder filled the template's OWN
               contents instead, so the two ends are joined here: every node the parse put in the contents is
               the shadow root's, in the order the author wrote them. Neither end is shared state — the shadow
               root was created a statement ago and the contents are this parse's own product — which is why
               this is the private move and not the capturing chokepoint. */
            for (child = contents->first_child; child; child = cnext) {
                cnext = child->next;
                dom_cow_move_private(contents, shadow, shadow, child);
            }
            /* "Insert a foreign element for templateStartTag, with HTML namespace and TRUE" — the true is
               `onlyAddToElementStack`, so the standard's template element is on the stack of open elements and
               in NO TREE. Lexbor's is in the tree; this is where it stops being. Discarded rather than left
               detached: nothing can ever reach it again, and `innerhtml-before-closing-tag.html` states the
               invariant as "the <template> element should never get added to the tree". */
            dom_cow_discard_private(root, n);
            /* "Set shadow's declarative to true" and "Set shadow's available to element internals to true". */
            shadow_root_mark_declarative(ctx, sr);
            /* §4.2.2.4 "assign slottables for a tree". A browser reaches this through §4.2.3's insertion steps
               as each node of the shadow tree is inserted; a parsed tree's nodes never pass through them, so
               the slots the parser just placed have never been asked what they hold — and what they hold is
               the HOST's children, which are already in place beside the template that just became this root. */
            slot_assign_for_a_tree(ctx, shadow);
            dsd_push(&todo, shadow);
            JS_FreeValue(ctx, sr);
        }
    }
    free(todo.v);
}

/* ---- declaration and installation --------------------------------------------------------------------------- */

/* The IDL name of each row, beside the content attribute it reflects, so the pair cannot drift. */
static const char *const DSD_ENUM_IDL[] = { "shadowRootMode", "shadowRootSlotAssignment" };

void declarative_shadow_init(JSContext *ctx)
{
    int i;

    DCHECK(!g_ready, "declarative_shadow_init ran twice — §4.12.3's members are declared once per AGENT");
    /* ONE SETTER BODY over the table, and one DECLARATION per row: the magic IS the row, and a setter carries
       its magic, so two rows sharing one id would write one attribute under two names. Declared here, at agent
       init, because a fresh id minted from a per-realm install is a member being minted per realm — which is
       what idl_declared_before_seal exists to catch. */
    for (i = 0; i < (int)(sizeof(DSD_ENUM) / sizeof(DSD_ENUM[0])); i++)
        g_id_enum_set[i] = idl_setter_id(ctx, IDL_DOMSTRING, false, js_tpl_enum_set, i);
    g_ready = 1;
}

void declarative_shadow_install_template_members(JSContext *ctx, JSValueConst template_proto)
{
    int i;

    DCHECK(g_ready, "§4.12.3's members were installed before declarative_shadow_init ran");
    for (i = 0; i < (int)(sizeof(DSD_ENUM) / sizeof(DSD_ENUM[0])); i++)
        idl_install_accessor(ctx, template_proto, DSD_ENUM_IDL[i], js_tpl_enum_get, i, g_id_enum_set[i]);
}

void declarative_shadow_free(void)
{
    int i;

    for (i = 0; i < (int)(sizeof(DSD_ENUM) / sizeof(DSD_ENUM[0])); i++)
        g_id_enum_set[i] = -1;
    g_ready = 0;
}
