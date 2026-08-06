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
 *     DocumentFragment includes NonElementParentNode;   // getElementById, node.c's, over this receiver
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
    DCHECK(!g_ready, "document_fragment_init ran twice — one instance is one document");
    g_frag_proto = JS_NewObjectProto(ctx, node_proto());
    CHECK(!JS_IsException(g_frag_proto), "DocumentFragment.prototype could not be allocated");
    idl_interface_tag(ctx, g_frag_proto, "DocumentFragment");
    node_install_parent_mixin(ctx, g_frag_proto);
    node_install_nonelement_parent_mixin(ctx, g_frag_proto);   /* §4.2.4, the same one Document includes */
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
