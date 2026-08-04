/* THE DocumentFragment INTERFACE — DOM §4.7.
 *
 * It was ABSENT, and the way it was absent is the shape this engine treats as worst: a fragment node wrapped as
 * a plain Node, so it HAD a wrapper and answered `nodeType === 11`, while every member that makes a fragment
 * useful — `children`, `querySelector`, `append`, `getElementById` — was undefined. Nothing threw. A page that
 * built a fragment, filled it and then queried it got undefined and took the branch behind it, and the engine
 * reported the surface that branch reaches instead of the one the page has.
 *
 * §4.7's IDL is four lines and three of them are includes:
 *     interface DocumentFragment : Node { constructor(); };
 *     DocumentFragment includes NonElementParentNode;   // getElementById
 *     DocumentFragment includes ParentNode;             // children, the element reads, prepend/append/
 *                                                       // replaceChildren, querySelector, querySelectorAll
 * So almost all of this file is two mixin installs. That is the point of having consolidated the ParentNode
 * mixin first: this interface exists by SAYING it includes the mixin, not by growing a third copy of members
 * Element and Document already have.
 *
 * IT IS CONSTRUCTIBLE, which none of the other node interfaces here are. `new DocumentFragment()` is real and a
 * page uses it to batch inserts, so the constructor builds a Lexbor fragment on the document and wraps it —
 * flow-private until the page inserts it, exactly like a clone. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/document_fragment.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/idl_args.h"

static JSValue g_frag_proto = JS_UNDEFINED;
static int     g_ready;

/* §4.2.4 NonElementParentNode.getElementById — the same question Document answers, over this fragment's subtree.
   A pure Lexbor walk over the id attribute: no page code, and the REAL element, wrapped once so identity holds.
   It is NOT Document's implementation reached with a different root, because Document's is `id` over the whole
   document and this one is over a subtree that is not in a document at all — same algorithm, different scope,
   and the scope is the whole of what NonElementParentNode says. */
static JSValue js_frag_get_element_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    lxb_dom_node_t *root = node_of(this_val), *n;
    const char *id;
    size_t idlen, vlen = 0;
    JSValue r = JS_NULL;

    (void)magic;
    if (!root || argc < 1) return JS_NULL;
    id = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!id) return JS_EXCEPTION;
    idlen = strlen(id);
    /* Pre-order over the fragment, which is tree order — §4.2.4 wants the FIRST such element. */
    for (n = root->first_child; n; ) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                                                (const lxb_char_t *)"id", 2, &vlen);
            if (v && vlen == idlen && memcmp(v, id, idlen) == 0) {
                r = node_wrap(ctx, n);
                break;
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) break;
        n = n->next;
    }
    JS_FreeCString(ctx, id);
    return r;
}

/* `constructor()` — §4.7. The fragment belongs to the document's memory, and is in no tree until the page puts
   it in one, so there is nothing to capture: an uninserted node is flow-private. */
static JSValue js_frag_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *root = document_root_node();
    lxb_dom_document_fragment_t *frag;

    (void)new_target; (void)argc; (void)argv;
    DCHECK(root != NULL, "new DocumentFragment() ran before the document existed");
    frag = lxb_dom_document_fragment_interface_create(root->owner_document);
    CHECK(frag != NULL, "DocumentFragment: the Lexbor fragment allocation failed — handing back a null the page "
                        "cannot tell from a node it never asked for is not an option");
    return node_wrap(ctx, lxb_dom_interface_node(frag));
}

void document_fragment_init(JSContext *ctx)
{
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    DCHECK(!g_ready, "document_fragment_init ran twice — one instance is one document");
    g_frag_proto = JS_NewObjectProto(ctx, node_proto());
    CHECK(!JS_IsException(g_frag_proto), "DocumentFragment.prototype could not be allocated");
    node_install_parent_mixin(ctx, g_frag_proto);
    idl_install_method(ctx, g_frag_proto, "getElementById", 1,
                       idl_method_id(ctx, ONE_STR, 1, js_frag_get_element_by_id, 0));
    /* CONSUMED by the table, which is what makes node_wrap hand a fragment this interface from now on. */
    node_set_proto(ctx, LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT, JS_DupValue(ctx, g_frag_proto));
    g_ready = 1;
}

void document_fragment_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "the DocumentFragment interface was installed before its prototype was built");
    /* A REAL CONSTRUCTOR, unlike every other node interface here — §4.7 declares one. */
    ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_frag_ctor, "DocumentFragment", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the DocumentFragment interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_frag_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "DocumentFragment", ctor);
}

JSValueConst document_fragment_proto(void)
{
    DCHECK(g_ready, "DocumentFragment.prototype was asked for before it was built");
    return g_frag_proto;
}

void document_fragment_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_frag_proto);
    g_frag_proto = JS_UNDEFINED;
    g_ready = 0;
}
