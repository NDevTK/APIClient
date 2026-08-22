/* THE DocumentType INTERFACE — DOM §4.6.
 *
 * It was ABSENT, in the way §4.7's DocumentFragment was absent before it: a doctype node the parser had built
 * WAS in the tree and WAS wrapped, as a plain Node, so `document.firstChild.nodeType === 10` held and every
 * member that says what a doctype IS — `name`, `publicId`, `systemId` — was undefined, and `instanceof
 * DocumentType` was a ReferenceError on an interface object that did not exist. Nothing threw; a page reading
 * `document.doctype.name` got undefined and took the branch behind it.
 *
 * §4.6's IDL is four lines:
 *     interface DocumentType : Node { readonly attribute DOMString name, publicId, systemId; };
 *     DocumentType includes ChildNode;
 * so this file is three getters over what Lexbor already stores and one mixin install. There is no constructor:
 * §4.6 declares none, and the way a page makes one is DOMImplementation.createDocumentType.
 *
 * WHY IT IS ITS OWN FILE rather than three cases in node.c: node.c is the BASE, and a member that only a
 * doctype has does not belong on Node.prototype — `element.publicId` must be undefined, which is what a
 * separate prototype is for. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document_type.h"
#include "core/dom/node.h"
#include "core/dom/node_heap.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* PER REALM — §3.7. The node-type table names the CLASS; the prototype lives in its per-context slot. */
static JSClassID g_doctype_class;
static int       g_ready;

/* §4.6's three strings. Lexbor holds the name as an interned attribute id and the two ids as plain strings, and
   its own accessors already answer "" for an unset one — which is what §4.6 says, since a doctype created with
   no public id has the EMPTY STRING and not null.
   magic 0 = name, 1 = publicId, 2 = systemId. */
static JSValue js_doctype_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_document_type_t *dt;
    const lxb_char_t *s;
    size_t len = 0;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE,
           "a DocumentType member ran on something that is not a doctype");
    dt = lxb_dom_interface_document_type(n);
    s = magic == 0 ? lxb_dom_document_type_name(dt, &len)
      : magic == 1 ? lxb_dom_document_type_public_id(dt, &len)
                   : lxb_dom_document_type_system_id(dt, &len);
    DCHECK(magic >= 0 && magic <= 2, "a DocumentType getter was declared with a magic this table does not name");
    return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewString(ctx, "");
}

void document_type_init(JSContext *ctx)
{
    JSClassDef d = { "DocumentType" };

    DCHECK(!g_ready, "document_type_init ran twice — the interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_doctype_class);
    JS_NewClass(JS_GetRuntime(ctx), g_doctype_class, &d);
    /* THE CLAIM is what makes node_wrap hand a doctype this interface from now on — in every realm, because
       what is claimed is the CLASS and each realm fills its own slot. */
    node_claim_type(LXB_DOM_NODE_TYPE_DOCUMENT_TYPE, g_doctype_class);
    g_ready = 1;
    realm_declare_intrinsic(document_type_install_proto);
}

void document_type_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    prev = JS_GetClassProto(ctx, g_doctype_class);
    DCHECK(JS_IsNull(prev), "document_type_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = node_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "DocumentType.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DocumentType");
    idl_install_accessor(ctx, proto, "name",     js_doctype_get, 0, -1);
    idl_install_accessor(ctx, proto, "publicId", js_doctype_get, 1, -1);
    idl_install_accessor(ctx, proto, "systemId", js_doctype_get, 2, -1);
    /* §4.6: `DocumentType includes ChildNode` — before/after/replaceWith/remove, over this receiver. */
    node_install_child_mixin(ctx, proto);
    JS_SetClassProto(ctx, g_doctype_class, proto);
}

void document_type_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto;

    DCHECK(g_ready, "the DocumentType interface was installed before its prototype was built");
    proto = JS_GetClassProto(ctx, g_doctype_class);
    DCHECK(!JS_IsNull(proto), "DocumentType.prototype was asked for in a realm that never ran its install");
    node_install_interface(ctx, global, "DocumentType", proto);
    JS_FreeValue(ctx, proto);
}

/* RELEASED BY ITS DECLARER — §4.6 is declared from document_init, so document_agent_free is what gives it back.
   IT TAKES NO RUNTIME because it holds no value and no atom: the prototype is each REALM's, released with its
   context, and what a C static holds for the agent is the class and the latch. Both go, because a class id kept
   past its runtime is exactly what core/agent_state.h found in dom_rect — a handle given back and then kept,
   whose only reader is the next agent's init deciding it need not run. */
void document_type_free(void)
{
    DCHECK(g_ready, "§4.6's DocumentType was released in an agent that never declared it");
    g_ready = 0;
    g_doctype_class = 0;
}

/* §4.6's DESTROY — see document_type.h for WHICH arena and why it is not lexbor's.
   THE HEADERS ARE COPIED BEFORE THE STRUCT GOES, because `public_id` and `system_id` ARE fields of the struct
   the node free is about to hand back; reading them afterwards is a read of freed memory, and it is the order
   `lxb_dom_document_type_interface_destroy` uses for exactly that reason. The arena is asked before it too, for
   the same reason — the answer is derived from `owner_document`, another field of the same struct. */
static void doctype_free_id(lxb_dom_document_t *doc, lexbor_str_t *id, const char *which)
{
    /* NULL OWNS NOTHING, AND THAT IS A STATE THE PARSER REALLY PRODUCES — this asserted the opposite first and
       `<!DOCTYPE html SYSTEM "about:legacy-compat">` fired it immediately. `lxb_html_token_doctype_parse`'s
       SYSTEM branch initialises `system_id`, appends, and RETURNS, so a system-only doctype never reaches the
       `set_pub_sys_empty` label and its `public_id` is left as the zeroed field the struct was calloc'd with.
       So this is not a hole to fill: absence of `data` is the POSITIVE statement that the header owns no bytes,
       which is the same statement `lexbor_str_destroy`'s own `if (str->data != NULL)` and node_heap.h's
       `str_owned` make from their two sides. */
    if (id->data == NULL)
        return;
    switch (node_heap_arena_of(id->data)) {
    case NODE_ARENA_NODES: (void) lexbor_str_destroy(id, doc->mraw, false); return;
    case NODE_ARENA_TEXT:  (void) lexbor_str_destroy(id, doc->text, false); return;
    case NODE_ARENA_NONE:
        break;
    }
    (void) which;
    DFAIL("a doctype's id was allocated out of neither of the agent's DOM arenas, so there is nowhere to give "
          "it back — every constructor DOM §4.6 has takes one of the two (the parser `mraw`, clone and "
          "createDocumentType `text`), so a fourth one was added without saying where its bytes live");
}

lxb_dom_interface_t *document_type_destroy(lxb_dom_document_type_t *dt)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(dt);
    lxb_dom_document_t *doc;
    lexbor_str_t public_id, system_id;

    DCHECK(dt != NULL, "no doctype was destroyed");
    DCHECK(node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE,
           "a node that is not a doctype reached §4.6's destroy — it would free two string headers out of "
           "whatever the other interface keeps at those offsets");
    DCHECK(node->owner_document != NULL && node->owner_document->mraw != NULL,
           "a doctype is being destroyed with its node document already detached from the agent's DOM heap — "
           "its two ids came out of that heap and there is now nowhere to give them back");
    doc = node->owner_document;
    public_id = dt->public_id;
    system_id = dt->system_id;
    (void) lxb_dom_node_interface_destroy(node);
    doctype_free_id(doc, &public_id, "publicId");
    doctype_free_id(doc, &system_id, "systemId");
    return NULL;
}
