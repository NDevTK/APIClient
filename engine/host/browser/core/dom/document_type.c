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

/* §4.6's THREE ATTRIBUTE NAMES, IN MAGIC ORDER, stated ONCE. The install below indexes this and hands the
   same string to §3.7.6's receiver test as the identifier its TypeError names, so the magic a getter is minted
   with and the identifier the property is keyed by cannot drift — which they could while the install spelled each name and the body switched on a bare 0/1/2. */
static const char *const DOCTYPE_MEMBERS[] = { "name", "publicId", "systemId" };

/* WEB IDL §3.7.6 "Attributes"' RECEIVER TEST IS STATED AT THE INSTALL AND PERFORMED BEFORE THIS BODY.
   §3.7.6's create an attribute getter says what a foreign receiver gets, in two steps: Web IDL §3.7.6
   "Attributes": "If jsValue does not implement target, then:", and its second arm, Web IDL §3.7.6
   "Attributes": "Otherwise, throw a TypeError."

   THIS FILE USED TO HOLD ITS OWN `doctype_receiver`, AND THE ARGUMENT FOR IT IS KEPT BECAUSE A READER WILL
   RE-DERIVE IT. It read: the three getters below converge on nothing that could ask that for them — minted by
   idl_install_accessor as plain JS_CFUNC_getter_magic functions with no pool entry, so core/idl_args.c's
   idl_implementation_check never runs and the receiver arrives exactly as the page wrote it. Every clause of
   that was true of the plain install form and is false of the one below: core/idl_args.h's
   idl_install_accessor_this takes the interface AT THE DECLARATION and performs §3.7's implementation check
   before the body, out of THIS FILE'S OWN document_type_is — so there is nothing here to write and nothing to
   keep in step. Writing it again would be the second answer to one question §A-FIX-OF-THE-FORM forbids.

   WHAT THE HELPER EXISTED TO STOP IS WHY IT MAY NOT COME BACK AS AN ASSERT. `DocumentType.prototype.name` is
   one expression, and DocumentType.prototype is an ordinary object of no node class — so node_of answered
   NULL, an assert fired in dev, and in release, where a DCHECK is compiled out, `lxb_dom_document_type_name`
   read `doc_type->node.owner_document->attrs` through that NULL. A forcing multi-path solver reaches members
   with unusual receivers constantly, which is what makes this a live cause of runs ending rather than a
   conformance detail. The refusal that replaces it is a THROW and lives in both builds.

   THE PREDICATE IS SHARED WITH THE ARGUMENT POSITION AND NOT RESTATED: document_type_is is already the
   IdlThisIs-shaped answer core/dom/dom_implementation.c narrows an ARGUMENT with
   (`idl_iface_narrow(document_type_is)`), so the receiver question and the §3.2.15 argument question are ONE
   answer and cannot come apart. */
/* §4.6's three strings. Lexbor holds the name as an interned attribute id and the two ids as plain strings, and
   its own accessors already answer "" for an unset one — which is what §4.6 says, since a doctype created with
   no public id has the EMPTY STRING and not null.
   magic 0 = name, 1 = publicId, 2 = systemId — DOCTYPE_MEMBERS is that order. */
static JSValue js_doctype_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_document_type_t *dt;
    const lxb_char_t *s;
    size_t len = 0;

    DCHECK(magic >= 0 && magic < (int)(sizeof DOCTYPE_MEMBERS / sizeof DOCTYPE_MEMBERS[0]),
           "a DocumentType getter was declared with a magic this table does not name");
    /* ON WHAT THE DECLARATION JUST READ, NEVER ON WHAT THE PAGE PASSED. Reaching this body means the install's
       document_type_is answered true, and that predicate IS this read of the node class plus the type test —
       so the two answers cannot disagree, and this asserts only that the install stated the interface. It is
       therefore not a dev-only guard in front of a release dereference: the refusal that stands in front of
       `n` is §3.7.6's TypeError and it is compiled into both builds. */
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE,
           "a §4.6 getter reached its body on a receiver that is not a DocumentType — its install states "
           "document_type_is, so reaching here means idl_install_accessor_this's own check did not run for it");
    dt = lxb_dom_interface_document_type(n);
    s = magic == 0 ? lxb_dom_document_type_name(dt, &len)
      : magic == 1 ? lxb_dom_document_type_public_id(dt, &len)
                   : lxb_dom_document_type_system_id(dt, &len);
    return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewString(ctx, "");
}

/* §3.2.15's type test — see document_type.h. `node_of` answers NULL for anything that is not a node wrapper,
   which is the same shape shadow_root_is_value has and for the same reason: the declaration hands this whatever
   the page passed. */
bool document_type_is(JSValueConst v)
{
    const lxb_dom_node_t *n = node_of(v);

    return n != NULL && n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE;
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
    /* KEYED BY THE SAME TABLE THE GETTER'S RECEIVER TEST INDEXES, so a magic and its identifier are one fact. */
    idl_install_accessor_this(ctx, proto, DOCTYPE_MEMBERS[0], js_doctype_get, 0, -1,
                              document_type_is, "DocumentType");
    idl_install_accessor_this(ctx, proto, DOCTYPE_MEMBERS[1], js_doctype_get, 1, -1,
                              document_type_is, "DocumentType");
    idl_install_accessor_this(ctx, proto, DOCTYPE_MEMBERS[2], js_doctype_get, 2, -1,
                              document_type_is, "DocumentType");
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
