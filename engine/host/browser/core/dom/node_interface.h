/* WHICH C STRUCT A NODE IS — and why lexbor cannot be asked that question twice.
 *
 * A real engine decides a node's interface ONCE, at creation, and the object carries it: Blink's
 * `Document::CreateElement` returns an `HTMLDivElement` or a bare `Element`, and the destructor is virtual, so
 * the question is asked once and can never be re-derived wrong. lexbor has no vtable — it emulates one with a
 * 2-D table indexed by `(local name, namespace)` and RE-DERIVES the interface at destruction from the ids the
 * node happens to carry. That is sound only while the key IDENTIFIES the interface, and it does not, in two
 * ways this engine reaches and upstream never can (upstream frees the whole arena and destroys no node on its
 * own; this engine's per-flow delta destroys exactly one node at a time, which is what made both visible):
 *
 * (1) A CREATOR THAT NEVER WRITES THE KEY. `lxb_dom_document_type_interface_create` callocs the struct and
 *     sets `node->type` and nothing else, so a DocumentType's `local_name` stays LXB_TAG__UNDEF — whose row
 *     holds `lxb_dom_element_interface_destroy`, while the row lexbor's table reserves for a doctype is
 *     LXB_TAG__EM_DOCTYPE. So every doctype is destroyed AS AN ELEMENT, and the fields line up disastrously
 *     rather than harmlessly: `lxb_dom_element_t.first_attr` shares an offset with
 *     `lxb_dom_document_type_t.system_id.data`, so the attribute walk begins at the arena pointer holding the
 *     doctype's systemId text and reads `attr->next` out of those characters.
 *     `document.implementation.createDocumentType("name", "publicId", "systemId")` is a wild free.
 * (2) A KEY VALUE WITH NO COLUMN. `lxb_html_interface_create` tests `ns < LXB_NS__LAST_ENTRY` before it
 *     indexes and falls back to a plain `lxb_dom_element_t`; `lxb_html_interface_destroy` indexes
 *     `res_destructor[local_name][ns]` — eight namespace columns — and does not test it. DOM §1.4 lets a page
 *     name ANY namespace, `dom_intern_namespace` mints an id for one lexbor's static table does not name, and
 *     a non-static ns id IS THE HASH ENTRY'S OWN ADDRESS. So `createElementNS("http://fake-namespace", "div")`
 *     loads a function pointer from roughly `&res_destructor[LXB_TAG_DIV][0] + address/8` and CALLS it.
 *
 * SO THE ENGINE OWNS THE DESTROY DISPATCH, on the field lexbor designed for it (`doc->destroy_interface`). It
 * answers by the thing that actually identifies the interface — the node's TYPE — and delegates every shape
 * for which lexbor's key IS an identity, because for those its create and its destroy already agree and a
 * second copy of a switch that is right is a second thing to keep in step.
 *
 * EVERY DOCUMENT HAS IT FROM BIRTH, WHICH IS WHY `dom_document_create` EXISTS. The first version of this
 * component installed the dispatcher on demand — at the line that mints an out-of-domain namespace id, and
 * again at §4.5 adopt, where such an id arrives without being minted. That was wrong in the way a trigger list
 * is always wrong: the doctype defect above needs no namespace at all, so it added a third trigger, and the
 * next one would have added a fourth. A document is not a thing you upgrade when you notice it needs it. The
 * on-demand installs are DELETED, `lxb_html_document_create` is not called anywhere in this engine, and
 * `dom_element_interface_create` asserts the property rather than establishing it.
 */
#ifndef APICLIENT_DOM_NODE_INTERFACE_H
#define APICLIENT_DOM_NODE_INTERFACE_H

#include <lexbor/dom/dom.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/ns/ns.h>
#include <lexbor/tag/tag.h>

/* THE ONE PLACE A DOCUMENT IS MADE. `lxb_html_document_create()` with this engine's node-interface dispatcher
   installed, and nothing else — an allocation failure comes back as lexbor returned it, because each caller's
   OOM sentence is its own measurement and not a policy this constructor may take over (core/frame/navigable.c's
   names `@HEAP`'s `childRealms` and says a Document is probably NOT what filled the heap). */
lxb_html_document_t *dom_document_create(void);

/* THE ONE PLACE A DOCUMENT IS DESTROYED, and it exists because `lxb_html_document_destroy` is no longer that
   place: a document's nodes come out of the AGENT's heap (core/dom/node_heap.h), so lexbor's destroy would
   free arenas every other document in this instance is still allocating out of. This one frees the nodes the
   document actually owns — one at a time, through the same per-interface destructors a single-node destroy
   uses — hands the arenas back, and then lets lexbor's destroy do what is still its: the four name hashes, the
   parser unref, and the document struct.
   THE NODES GO BEFORE THE ARENAS, which is the same ordering the per-flow delta already states for its own two
   passes (solver/dom_cow.c's kind-4-before-kind-5 release): a node freed after its arena is a read of memory
   that is gone. */
void dom_document_destroy(lxb_html_document_t *dom);

/* THE ONE PLACE AN ELEMENT INTERFACE IS MADE — `lxb_dom_document_create_interface` with the invariant above
   asserted at it. Both callers (DOM §4.5's storage step in core/dom/element.c, §4.4's clone in
   core/dom/node.c) go through it, and it is where a document that bypassed `dom_document_create` is caught:
   without the assert the element is built fine and the process dies at an indirect call an arbitrary number
   of destroys later, in a frame naming neither the document nor the element. */
lxb_dom_interface_t *dom_element_interface_create(lxb_dom_document_t *doc, lxb_tag_id_t tag, lxb_ns_id_t ns);

/* Whether the nodes `doc` owns destroy through this engine's dispatcher. It resolves through
   `lxb_dom_document_owner(doc)`, because an INHERITED document (HTML §13.4's fragment parser, element.c's
   `own_context` document — `lxb_dom_document_init` with a non-NULL owner) makes `lxb_dom_node_interface_create`
   stamp every node it builds with the OWNER, so it is the owner's field the destroy reads; that resolution
   answers `doc` itself for an original document. */
int dom_document_owns_node_interfaces(lxb_dom_document_t *doc);

#endif
