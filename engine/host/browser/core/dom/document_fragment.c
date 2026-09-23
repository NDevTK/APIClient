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
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/dom/document_fragment.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/realm.h"
#include "core/idl_args.h"
#include "solver/dom_cow.h"   /* dom_cow_note_created — a created node belongs to the flow's delta */

/* PER REALM — §3.7. The node-type table names the CLASS; the prototype lives in its context slot. */
static JSClassID g_frag_class;
static int     g_ready;

/* `constructor()` — §4.7. There is nothing to CAPTURE, because being in no tree means nothing shared changed;
   but the fragment belongs to the document's Lexbor memory, so the flow that made it must OWN it. Those are two
   different statements and only the first one was here. A fragment is emptied by the insertion that consumes it
   and never becomes reachable from the tree, so without the creation entry every one ever built stays in the
   document's arena for the life of the instance — with no owner, and invisible to the runtime's gc_obj_list
   walk, which only sees GC objects. */
static JSValue js_frag_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    lxb_dom_document_t *doc = document_associated(ctx);
    lxb_dom_document_fragment_t *frag;

    (void)new_target; (void)argc; (void)argv;
    /* §4.7's own step is "set this's node document to current global object's associated Document", so the
       operand is the DOCUMENT — asking through the document element answers null for a document whose element
       has been removed, which is a Document that is perfectly present (see document_associated). A `CHECK`
       rather than a should-never-happen because the pointer is read through in the shipped build, which is the
       same reason the allocation below is one. */
    CHECK(doc != NULL, "new DocumentFragment() ran in a realm with no associated Document");
    frag = lxb_dom_document_fragment_interface_create(doc);
    CHECK(frag != NULL, "DocumentFragment: the Lexbor fragment allocation failed — handing back a null the page "
                        "cannot tell from a node it never asked for is not an option");
    dom_cow_note_created(lxb_dom_interface_node(frag));   /* this flow made it: the delta owns it */
    return node_wrap(ctx, lxb_dom_interface_node(frag));
}

void document_fragment_init(JSContext *ctx)
{
    JSClassDef d = { "DocumentFragment" };

    DCHECK(!g_ready, "document_fragment_init ran twice — the interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_frag_class);
    JS_NewClass(JS_GetRuntime(ctx), g_frag_class, &d);
    /* THE CLAIM is what makes node_wrap hand a fragment this interface from now on — in every realm, because
       what is claimed is the CLASS and each realm fills its own slot. */
    node_claim_type(LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT, g_frag_class);
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The row is `document` and
       NOT this file: §4.7 is declared from document_init, so document_agent_free is the release that
       reaches this one, and a sub-component names the row that releases it. The class id was minted
       inside core/platform.c's declare column and named to no declaration, which is the direction that
       file's conservation identity aborts on — a handle nothing gives back, read by the next agent's
       own init as a latch. */
    agent_state_class("document", &g_frag_class,
                      "DOM §4.7's DocumentFragment class, claimed for the fragment node type");
    realm_declare_intrinsic(document_fragment_install_proto);
}

/* §4.7's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
void document_fragment_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    prev = JS_GetClassProto(ctx, g_frag_class);
    DCHECK(JS_IsNull(prev), "document_fragment_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = node_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "DocumentFragment.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DocumentFragment");
    node_install_parent_mixin(ctx, proto);
    node_install_nonelement_parent_mixin(ctx, proto);   /* §4.2.4, the same one Document includes */
    JS_SetClassProto(ctx, g_frag_class, proto);
}

void document_fragment_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "the DocumentFragment interface was installed before its prototype was built");
    /* A REAL CONSTRUCTOR, unlike every other node interface here — §4.7 declares one. */
    ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_frag_ctor, "DocumentFragment", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the DocumentFragment interface object could not be allocated");
    {
        JSValue proto = document_fragment_proto(ctx);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    idl_define_global_property_reference(ctx, global, "DocumentFragment", ctor);
}

JSValue document_fragment_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_frag_class);
    DCHECK(!JS_IsNull(proto), "DocumentFragment.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

/* RELEASED BY ITS DECLARER. §4.7 is declared from document_init and was being released from element_free's
   cascade — a release undoing another row's work — so it is reached from document_agent_free now.
   IT TAKES NO RUNTIME because it gives back no value and no atom: the prototypes are the REALMS', released with
   their contexts, and what this component holds for the agent is the CLASS and the latch. The class was the
   half that was missing — cleared here now, because a class id kept past its runtime is what core/agent_state.h
   found in dom_rect and dom_rect_list, and the only reader of a stale one is the next agent's init deciding it
   need not run. */
void document_fragment_free(void)
{
    /* NOT `if (!g_ready) return;` — the release is the inverse of a declaration that is unconditional, so the
       test could never be true and could only hide a release that had not finished. */
    DCHECK(g_ready, "§4.7's DocumentFragment was released in an agent that never declared it");
    g_ready = 0;
    /* THE CLASS IS NOT PUT BACK HERE ANY MORE. It is declared under `document`, whose release ends in
       agent_state_undo — one reset, computed from the registry that already holds this slot's address
       and its kind. A line here as well would be a SECOND resetter beside that one, which is the pair
       core/agent_state.h's undo exists to stop being kept by hand. */
    /* AND THE CASCADE REACHED THIS FILE — the claim that entitles document_agent_free's last line to put
       this file's slot back. See core/agent_state.h's agent_state_reached. */
    agent_state_reached("document");
}
