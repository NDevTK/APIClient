/* THE DocumentType INTERFACE — DOM §4.6. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_TYPE_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* THE AGENT'S HALF: the class, the node-type claim, and the per-realm prototype declaration. */
void document_type_init(JSContext *ctx);
/* §4.6's interface prototype object for ONE realm — declared into core/realm.h's list. */
void document_type_install_proto(JSContext *ctx);
/* The interface OBJECT on a realm's global, so `doctype instanceof DocumentType` holds. */
void document_type_install(JSContext *ctx, JSValueConst global);
/* Reached from document_agent_free — §4.6 is declared by document_init, so it is released by its declarer. */
void document_type_free(void);

/* WEB IDL §3.2.15 Interface types' "If V implements I" OVER THIS INTERFACE, so an IDL position declared
   `DocumentType` can state its brand in its DECLARATION. Every node wrapper shares one class, so
   `idl_iface_brand(node_class_id())` says only "a Node" and `idl_iface_narrow(document_type_is)` is what says
   which kind — the same pairing core/dom/shadow_root.h states and slot.c, element_internals.c and
   intersection_observer.c already declare. DOM §4.5.1 Interface DOMImplementation's
   `optional DocumentType? doctype = null` is what needed it: the class test alone crossed
   `createDocument(null, "x", document.createElement("div"))` as itself, where §3.2.15 owes a TypeError.
   Side-effect-free; answers false for anything that is not a node. */
bool document_type_is(JSValueConst v);

/* §4.6'S TWO IDS ARE FREED OUT OF THE ARENA THEY WERE ALLOCATED FROM, WHICH IS NOT THE ONE LEXBOR FREES THEM
 * INTO. `lxb_html_token_doctype_parse` takes `doc_type->node.owner_document->mraw` and calls
 * `lexbor_str_init(&public_id, mraw, 0)` and the same for `system_id` — an allocation of one byte each, out of
 * the NODE arena, on EVERY doctype, because §13.2.6.1 gives a doctype with no ids the EMPTY STRING rather than
 * nothing. `lxb_dom_document_type_interface_destroy` then reads `owner_document->text` and frees both there.
 * Upstream never notices: it destroys both arenas together at document teardown and asks neither for a count.
 *
 * THIS ENGINE CANNOT ABSORB IT, for the two reasons core/dom/node_heap.h exists. The arenas are the AGENT'S and
 * outlive every document, so the bytes leak into `g_nodes` for the life of the agent rather than being dropped
 * with a document; and `lexbor_mraw_free` VALIDATES NOTHING — it inserts the pointer into that arena's
 * size-keyed free cache and decrements its `ref_count` — so `g_text`'s cache comes to hold a pointer into
 * `g_nodes`' chunks and hands it out on the next `g_text` allocation of that size. Two arenas then alias, which
 * is a node's storage and a string's bytes at one address with nothing to say so. The ref_count that goes
 * NEGATIVE is the symptom node_heap.c's teardown assertion prints; the aliasing is the defect.
 *
 * AND THE ARENA CANNOT SIMPLY BE THE OTHER ONE, which is the trap this fix walks into if it stops at the
 * parser. §4.6 has THREE constructors and they do not agree: `lxb_dom_document_type_interface_clone` and
 * `lxb_dom_document_type_create` (the one §4.5.1's createDocumentType reaches) both allocate the ids from
 * `text`, and only the parser uses `mraw`. Freeing every doctype into `mraw` would fix the parsed one and
 * break the other two in the same direction, which is why the destroy ASKS — core/dom/node_heap.h's
 * `node_heap_arena_of` answers exactly, and an id belonging to neither arena is a `DFAIL` rather than a guess.
 *
 * SO THE DESTROY IS OURS. It is the same sequence as lexbor's — copy the two string headers BEFORE the struct
 * is freed, since the struct is where they live — with the arena asked per id instead of named once. */
lxb_dom_interface_t *document_type_destroy(lxb_dom_document_type_t *dt);

#endif
