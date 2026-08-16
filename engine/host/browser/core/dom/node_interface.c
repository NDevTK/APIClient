/* See node_interface.h. */
#include <lexbor/html/html.h>
#include <lexbor/html/interface.h>
#include <lexbor/dom/interfaces/element.h>

#include "check.h"
#include "core/dom/node_interface.h"

/* THE MISSING COLUMN OF lexbor's OWN TABLE, and nothing else. The condition is not "a namespace this engine
   does not like" — it is the EXACT pair `lxb_html_interface_create` routes to `lxb_dom_element_interface_create`
   and `lxb_html_interface_destroy` then looks up in a table that has no column for it. Written as the create
   side's own test so the two read as one statement:

     tag_id < LXB_TAG__LAST_ENTRY  &&  ns >= LXB_NS__LAST_ENTRY   ->   plain lxb_dom_element_t

   Every other shape falls through to lexbor's dispatcher unchanged, because for every other shape its create
   and its destroy already agree, and a second copy of a switch that is right is a second thing to keep in
   step. A node type other than ELEMENT never reaches the table's arm with a page-supplied namespace: an Attr
   is destroyed by the switch's own ATTRIBUTE case, and Text, Comment, CDATASection, ProcessingInstruction,
   DocumentType and DocumentFragment are created by lexbor with LXB_TAG__UNDEF and LXB_NS_HTML, both static. */
static lxb_dom_interface_t *dom_node_interface_destroy(lxb_dom_interface_t *intrfc)
{
    lxb_dom_node_t *node = intrfc;

    if (node == NULL)
        return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
        node->local_name < LXB_TAG__LAST_ENTRY && node->ns >= LXB_NS__LAST_ENTRY)
        return lxb_dom_element_interface_destroy(intrfc);
    return lxb_html_interface_destroy(intrfc);
}

/* THE DISPATCHER THAT WILL BE CONSULTED IS THE ONE ON THE DOCUMENT THAT OWNS THE NODES, and for an INHERITED
   document that is not the document you are holding. `lxb_dom_document_init` with a non-NULL owner — which is
   how HTML §13.4's fragment-parsing document and element.c's `own_context` document are built — points the new
   document's node at the OWNER, and `lxb_dom_node_interface_create` then stamps every node it makes with
   `lxb_dom_document_owner(document)`. So a node "created in" the temporary document is destroyed through the
   REAL one's `destroy_interface`, and installing on the temporary struct would have installed on the one
   pointer nothing ever reads. `lxb_dom_document_owner` answers the document itself when it is original, so
   this is one resolution and not a branch. */
static lxb_dom_document_t *dom_document_of_nodes(lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "the owner of no document was asked for");
    return lxb_dom_document_owner(doc);
}

int dom_document_owns_node_interfaces(lxb_dom_document_t *doc)
{
    return doc != NULL && dom_document_of_nodes(doc)->destroy_interface == dom_node_interface_destroy;
}

void dom_document_own_node_interfaces(lxb_dom_document_t *doc)
{
    lxb_dom_document_t *owner;

    DCHECK(doc != NULL, "this engine's node-interface dispatcher was installed on no document");
    owner = dom_document_of_nodes(doc);
    /* EVERY document in this engine is an lxb_html_document_t, so the only thing this may replace is lexbor's
       HTML dispatcher or itself. A third one means a document was built by something this file has never seen,
       and installing over it would silently drop whatever that dispatcher knew. */
    DCHECK(owner->destroy_interface == lxb_html_interface_destroy ||
           owner->destroy_interface == dom_node_interface_destroy,
           "a document destroys its nodes through a dispatcher that is neither lexbor's HTML one nor this "
           "engine's — name the third one here before it is overwritten");
    owner->destroy_interface = dom_node_interface_destroy;
}

lxb_dom_interface_t *dom_element_interface_create(lxb_dom_document_t *doc, lxb_tag_id_t tag, lxb_ns_id_t ns)
{
    DCHECK(doc != NULL, "an element interface was created in no document");
    /* THE TWO-SIDED HALF. A non-static namespace id is one `dom_intern_namespace` minted, and that function
       takes the obligation on the line that mints — so by the time an element is built with one, the document
       already destroys its own nodes. If it does not, the element is created fine and the process dies at an
       indirect call through `res_destructor[tag][address]` an arbitrary number of destroys later, which is
       the crash this assert exists to move back to here. */
    DCHECK(ns < LXB_NS__LAST_ENTRY || dom_document_owns_node_interfaces(doc),
           "an element is being created with a namespace outside lexbor's static table in a document that "
           "still destroys its nodes through lexbor's dispatcher — that dispatcher indexes an eight-column "
           "destructor table with this namespace id, which is a heap address");
    return lxb_dom_document_create_interface(doc, tag, ns);
}
