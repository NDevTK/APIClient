/* See node_interface.h. */
#include <lexbor/html/html.h>
#include <lexbor/html/interface.h>
#include <lexbor/dom/interfaces/document_type.h>
#include <lexbor/dom/interfaces/element.h>

#include "check.h"
#include "core/dom/node_heap.h"
#include "core/dom/node_interface.h"

/* THE TWO ARMS ARE THE TWO WAYS lexbor's KEY IS NOT AN IDENTITY — see node_interface.h for the measurement of
 * each. The doctype is answered by TYPE, which covers one whatever built it: this engine's createDocumentType,
 * the HTML parser's, or lexbor's own `lxb_dom_document_type_interface_clone`, which copies the same zero. The
 * element arm is written as the CREATE side's own test (`tag < LXB_TAG__LAST_ENTRY && ns >= LXB_NS__LAST_ENTRY`
 * is exactly the branch where `lxb_html_interface_create` falls back to a plain `lxb_dom_element_t`) so the two
 * halves read as one statement.
 *
 * EVERYTHING ELSE IS LEXBOR'S, AND THE DCHECK IS WHAT MAKES THAT A STATEMENT RATHER THAN A HOPE. Five node
 * types reach the table: TEXT, COMMENT, ELEMENT, DOCUMENT and DOCUMENT_TYPE (an Attr, a CDATASection, a
 * ProcessingInstruction and a DocumentFragment each get their own case in lexbor's switch and never index it).
 * Of the three left here, each one's creator DOES write the tag its row expects —
 * `lxb_dom_document_create_text_node` and `_create_comment` pass it to create-interface, `lxb_dom_document_init`
 * assigns LXB_TAG__DOCUMENT — so the assert is the standing check that the next creator to forget crashes with
 * a name instead of running another interface's destructor over these bytes. */
static lxb_dom_interface_t *dom_node_interface_destroy(lxb_dom_interface_t *intrfc)
{
    lxb_dom_node_t *node = intrfc;

    if (node == NULL)
        return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
        return lxb_dom_document_type_interface_destroy(intrfc);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
        node->local_name < LXB_TAG__LAST_ENTRY && node->ns >= LXB_NS__LAST_ENTRY)
        return lxb_dom_element_interface_destroy(intrfc);
    DCHECK((node->type != LXB_DOM_NODE_TYPE_TEXT     || node->local_name == LXB_TAG__TEXT) &&
           (node->type != LXB_DOM_NODE_TYPE_COMMENT  || node->local_name == LXB_TAG__EM_COMMENT) &&
           (node->type != LXB_DOM_NODE_TYPE_DOCUMENT || node->local_name == LXB_TAG__DOCUMENT),
           "a node is about to be destroyed through lexbor's (local name, namespace) table with a local name "
           "its creator never wrote — the table answers with another interface's destructor, which is how a "
           "doctype came to be freed as an element; answer this type by its TYPE, above");
    return lxb_html_interface_destroy(intrfc);
}

/* An INHERITED document (§13.4's fragment parser, element.c's `own_context` document) is built by
   `lxb_dom_document_init` with a non-NULL owner, which points its document node at the OWNER; every node
   `lxb_dom_node_interface_create` then makes is stamped with `lxb_dom_document_owner(document)`. So the field
   the destroy reads belongs to the owner, and installing on the inherited struct would write the one pointer
   nothing reads. `lxb_dom_document_owner` answers the document itself when it is original, so this is one
   resolution and not a branch. */
static lxb_dom_document_t *dom_document_of_nodes(lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "the owner of no document was asked for");
    return lxb_dom_document_owner(doc);
}

int dom_document_owns_node_interfaces(lxb_dom_document_t *doc)
{
    return doc != NULL && dom_document_of_nodes(doc)->destroy_interface == dom_node_interface_destroy;
}

/* IT DOES NOT DECIDE THE FAILURE POLICY, deliberately: an allocation failure is returned exactly as lexbor
   returned it, so each caller keeps the sentence it already had. `core/frame/navigable.c`'s is the reason —
   its OOM message is not about the document at all, it is the heap-diagnosis note telling the reader to check
   `@HEAP`'s `childRealms` before believing a Document is what filled the heap, and a constructor shared by
   seventeen callers is the wrong place for one caller's measurement. */
lxb_html_document_t *dom_document_create(void)
{
    lxb_html_document_t *dom = lxb_html_document_create();
    lxb_dom_document_t *doc;

    if (dom == NULL)
        return NULL;
    doc = dom_document_of_nodes(lxb_dom_interface_document(dom));
    /* EVERY document in this engine is an lxb_html_document_t, so the only thing this may replace is lexbor's
       HTML dispatcher. A third one means a document was built by something this file has never seen, and
       installing over it would silently drop whatever that dispatcher knew. */
    DCHECK(doc->destroy_interface == lxb_html_interface_destroy,
           "a freshly created document already destroys its nodes through a dispatcher that is not lexbor's "
           "HTML one — name the third one here before it is overwritten");
    doc->destroy_interface = dom_node_interface_destroy;
    /* AND THE ARENAS ARE THE AGENT'S, from here rather than from the first adopt — see core/dom/node_heap.h.
       Here, because it is the only moment the document has none of its own bytes yet, and because a document
       that reached its first node with private arenas can never be given the agent's afterwards. */
    node_heap_attach(doc);
    return dom;
}

void dom_document_destroy(lxb_html_document_t *dom)
{
    lxb_dom_document_t *doc;
    lxb_dom_node_t *child;

    DCHECK(dom != NULL, "no document was destroyed");
    doc = lxb_dom_interface_document(dom);
    DCHECK(dom_document_owns_node_interfaces(doc),
           "a document is being destroyed that does not destroy its nodes through this engine's dispatcher — "
           "it was not built by dom_document_create, so it never took the agent's heap and this entry would "
           "hand back arenas it does not hold");
    /* `lxb_dom_node_destroy_deep` DETACHES each node before it frees it, so the document's child list drains
       as the loop runs; it is iterative, so the depth of the page's markup costs no C stack. */
    while ((child = doc->node.first_child) != NULL)
        lxb_dom_node_destroy_deep(child);
    node_heap_detach(doc);
    lxb_html_document_destroy(dom);
}

lxb_dom_interface_t *dom_element_interface_create(lxb_dom_document_t *doc, lxb_tag_id_t tag, lxb_ns_id_t ns)
{
    DCHECK(doc != NULL, "an element interface was created in no document");
    /* THE PROPERTY IS ASSERTED HERE, NOT ESTABLISHED HERE — see node_interface.h on why an on-demand install
       is the wrong shape. A document reaching this line without the dispatcher was built by something that
       bypassed dom_document_create, and every node it ever destroys is destroyed by whatever lexbor's table
       happens to hold at the key it re-derives. */
    DCHECK(dom_document_owns_node_interfaces(doc),
           "an element is being created in a document that still destroys its nodes through lexbor's "
           "dispatcher — it was not built by dom_document_create, which is the only place a Document is made");
    return lxb_dom_document_create_interface(doc, tag, ns);
}
