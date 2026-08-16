/* WHICH C STRUCT A NODE IS — the create/destroy pair, and the one pair lexbor answers twice differently.
 *
 * A real engine decides a node's interface ONCE, at creation, and the object carries it: Blink's
 * `Document::CreateElement` returns an `HTMLDivElement` or a bare `Element` and the destructor is virtual, so
 * the question is asked once and can never be re-derived wrong. lexbor has no vtable — it emulates one with a
 * 2-D table indexed by `(local name, namespace)` and RE-DERIVES the interface at destruction from the ids the
 * node carries. That is sound only while the key's DOMAIN is closed, and this engine is exactly the thing that
 * opens it: DOM §1.4 lets a page name ANY namespace, `dom_intern_namespace` mints an id for one lexbor's static
 * table does not name, and a non-static id IS THE HASH ENTRY'S OWN ADDRESS.
 *
 * lexbor's two halves then disagree about that id, and only one of them says so:
 *
 *   lxb_html_interface_create:   tag < LXB_TAG__LAST_ENTRY
 *                                  ? (ns < LXB_NS__LAST_ENTRY ? res_constructors[tag][ns]
 *                                                             : lxb_dom_element_interface_create)
 *                                  : (three-way on ns)
 *   lxb_html_interface_destroy:  local_name < LXB_TAG__LAST_ENTRY
 *                                  ? res_destructor[local_name][ns]        <-- ns UNCHECKED
 *                                  : (three-way on ns)
 *
 * `res_destructor` is `[LXB_TAG__LAST_ENTRY][LXB_NS__LAST_ENTRY]`, i.e. eight columns. So
 * `document.createElementNS("http://fake-namespace", "div")` — a static tag id and a namespace id that is a
 * heap address — is created as a plain `lxb_dom_element_t` and destroyed by loading a function pointer from
 * roughly `&res_destructor[LXB_TAG_DIV][0] + address/8` and CALLING it. That is not a wrong answer, it is a
 * jump to whatever byte pattern lives there: `wpt.mjs domparsing` reported `createContextualFragment.html` as
 * SIGSEGV, and the two `createElementNS("http://fake-namespace", …)` calls at that file's top level are the
 * whole of it.
 *
 * SO THE ENGINE OWNS THE DESTROY DISPATCH, on the field lexbor designed for it (`doc->destroy_interface`), and
 * it answers exactly the one pair lexbor's create routed to `lxb_dom_element_interface_create` and its destroy
 * cannot reach. Every other pair is lexbor's, unchanged, because for every other pair the two halves already
 * agree — this file is not a second dispatcher beside lexbor's, it is the missing column of lexbor's own.
 *
 * WHEN A DOCUMENT MUST HAVE IT: the moment it can hold a node whose namespace id is outside lexbor's static
 * domain. There are exactly two ways that happens, and each calls dom_document_own_node_interfaces:
 *   - the id is BORN there — `dom_intern_namespace` is the only thing in this engine that mints one, so it is
 *     the only thing that can create the obligation, and it takes it on the line that mints;
 *   - the id ARRIVES there — DOM §4.5 adopt re-points a node's node document without re-interning its names,
 *     so a node created in document A is destroyed through document B's dispatcher.
 * `dom_element_interface_create` is the other side of that assertion: an element built with a non-static
 * namespace in a document that does not yet destroy its own nodes is the state this file exists to make
 * impossible, so it CRASHES there rather than at the indirect call three destroys later.
 */
#ifndef APICLIENT_DOM_NODE_INTERFACE_H
#define APICLIENT_DOM_NODE_INTERFACE_H

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>
#include <lexbor/tag/tag.h>

/* This engine's element creation — `lxb_dom_document_create_interface` with the invariant above asserted at it.
   Both callers (DOM §4.5's storage step in core/dom/element.c, §4.4's clone in core/dom/node.c) go through it,
   and there is no other way an element interface is built here. */
lxb_dom_interface_t *dom_element_interface_create(lxb_dom_document_t *doc, lxb_tag_id_t tag, lxb_ns_id_t ns);

/* Install this engine's destroy dispatcher for the nodes `doc` owns. Idempotent, and it asserts that what it
   replaces is lexbor's HTML one — a third dispatcher would mean a document built by something this file has
   not seen. It installs on `lxb_dom_document_owner(doc)`, because an INHERITED document (§13.4's fragment
   parser, element.c's `own_context`) stamps its nodes with the OWNER and it is the owner's field that the
   destroy reads; that resolution answers `doc` itself for an original document. */
void dom_document_own_node_interfaces(lxb_dom_document_t *doc);
/* Whether the nodes `doc` owns already destroy through it — the read half of the assertion above. */
int dom_document_owns_node_interfaces(lxb_dom_document_t *doc);

#endif
