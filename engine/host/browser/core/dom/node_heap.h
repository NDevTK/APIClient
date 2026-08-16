/* WHERE A NODE'S BYTES LIVE — THE AGENT'S HEAP, AND NOT THE DOCUMENT'S.
 *
 * WHAT THIS IS. Every node in this engine is `lexbor_mraw_calloc(document->mraw, …)` and every string it owns
 * comes out of `document->text`, and `lxb_dom_node_interface_destroy` frees it with
 * `lexbor_mraw_free(node->owner_document->mraw, node)`. That is sound for exactly as long as a node's node
 * document never changes — and DOM §4.5's adopt is the operation whose entire purpose is to change it
 * ("for each inclusiveDescendant … set inclusiveDescendant's node document to document",
 * https://dom.spec.whatwg.org/#concept-node-adopt). After one adopt the two halves are each broken and they
 * break differently:
 *
 *   (1) THE FREE GOES TO THE WRONG ALLOCATOR. `lexbor_mraw_free` does not validate: it inserts the chunk into
 *       THAT mraw's size-keyed free cache and decrements THAT mraw's `ref_count`. So the destination's cache
 *       comes to hold a pointer into the source's chunks, and the destination hands it out on the next
 *       allocation of that size. `lxb_dom_attr_set_value` and `lxb_dom_character_data_replace` read the arena
 *       off `owner_document` the same way, so it is not only the destroy.
 *   (2) THE ARENA IS FREED WHILE THE NODE IS LIVE. `lxb_dom_document_destroy` on the SOURCE does
 *       `lexbor_mraw_destroy(document->text, true)` and `lexbor_mraw_destroy(document->mraw, true)`, which
 *       return every chunk to the system allocator — while the adopted node, now reachable from the
 *       DESTINATION's tree, still sits in them. `d = createHTMLDocument(); document.body.appendChild(d.body)`
 *       is enough, and a use-after-free has no crash site of its own: the read succeeds until the allocator
 *       reuses the page.
 *
 * WHY THE STORAGE IS NOT MOVED. Everything a node owns EXCEPT the node itself is reachable only through the
 * node, so it could be copied into the destination's arenas and freed from the source's. The node's interface
 * STRUCT cannot: its ADDRESS is its identity. Its parent, its siblings and its children name it; so do the
 * per-flow COW delta's entries, the wrapper identity map, the attribute list's `owner`, a Range's boundary
 * points and a `<template>`'s `host`. A copy at another address is a different node, and `===` says so. So
 * "move the bytes with the document" is answerable for seven of the eight things a node owns and unanswerable
 * for the eighth, which means it is not the fix.
 *
 * THE FIX IS THE HEAP BOUNDARY, AND THE SPEC ALREADY STATES IT. A real engine does not put node memory under a
 * Document: Blink allocates every `Node` on the Oilpan heap of the AGENT and adopt is a pointer write.
 * CLAUDE.md says the same thing for this engine's own boundary — an instance IS an origin-keyed agent cluster
 * because that is the spec's heap boundary, and HTML then RELIES on it (`iframe.contentDocument.body
 * .appendChild(subframe)` inserts a node the other document made). A per-DOCUMENT arena is a lexbor artifact
 * that upstream gets away with because upstream destroys a whole document at once and never adopts across one.
 * So every Document in this agent allocates its nodes and its text out of ONE pair of arenas, and both halves
 * above stop existing rather than being repaired: the destination's mraw IS the source's, and no document's
 * destroy can free memory another document's nodes live in.
 *
 * WHAT A DOCUMENT'S DESTROY THEN IS. It frees its own nodes, one at a time, through the same per-interface
 * destructors a single-node destroy already uses, and it hands the arenas back to the agent — see
 * `dom_document_destroy` in core/dom/node_interface.h, which is the one place that runs. The arenas outlive
 * every document and are destroyed with the LAST one, which is where the leak assertion below lives.
 *
 * THE NAME HASHES ARE NOT HERE, DELIBERATELY. `tags`, `ns`, `prefix` and `attrs` stay per document and a name
 * id keeps travelling through core/dom/name_intern.h's import at the same funnel. A name is a value the
 * standard defines per document (§4.9's key is (namespace, local name), and two documents may hold the same
 * bytes), whereas a node's storage is not a value at all — and the ids also have to cross an INSTANCE, where
 * bytes cross and pointers cannot. */
#ifndef APICLIENT_DOM_NODE_HEAP_H
#define APICLIENT_DOM_NODE_HEAP_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Point `doc` at the agent's heap and destroy the private arenas `lxb_dom_document_init` just made for it.
   Called by `dom_document_create` BEFORE a single node exists in the document — which the assertion inside
   states, because a document that has already allocated would have those bytes freed under it. */
void node_heap_attach(lxb_dom_document_t *doc);

/* Give up `doc`'s claim. Called by `dom_document_destroy` AFTER the document's own nodes are gone and BEFORE
   lexbor's destroy runs, and it NULLs `doc->mraw` and `doc->text` — `lexbor_mraw_destroy(NULL, true)` is a
   no-op, which is what lets lexbor's own destroy keep doing the four hash destroys, the parser unref and the
   struct free without also freeing the agent's heap out from under every other document. */
void node_heap_detach(lxb_dom_document_t *doc);

/* THE INVARIANT THE ABOVE ESTABLISHES, as a predicate rather than a comment — the arena half's twin of
 * name_intern.h's `dom_names_owned_by`: a node whose node document is `doc` holds no byte belonging to any
 * other document's arenas. It is EXACTLY checkable, because `lexbor_mraw_t` can answer whether a pointer is
 * one of its chunks: `mraw->mem` is a doubly-linked list of `lexbor_mem_chunk_t`, each with its `data` base
 * and its `size`, and every allocation is inside exactly one of them. That costs a walk of the chunk list per
 * pointer rather than O(1), which is why it is asked only from DCHECKs.
 *
 * IT DISPATCHES ON `node->type`, because WHICH bytes a node owns differs by kind and the list is derived from
 * lexbor's own destructors — the authoritative statement of what a node owns is what its destructor frees:
 *   - EVERY kind: the interface struct           (`lxb_dom_node_interface_destroy` → `doc->mraw`)
 *   - Attr:       `value`                        (`lxb_dom_attr_interface_destroy` → `doc->mraw`)
 *                 `value->data`                                                    → `doc->text`
 *   - Element:    `is_value` and `is_value->data` — which NOTHING in this engine sets and NOTHING in lexbor
 *                 frees, so the arm asserts its absence instead of walking it (see the case).
 *   - Text, Comment, CDATASection, ProcessingInstruction:
 *                 `char_data.data.data`          (`lxb_dom_character_data_interface_destroy` → `doc->text`)
 *   - ProcessingInstruction, additionally: `target.data`                            → `doc->text`
 *   - DocumentType: `public_id.data`, `system_id.data`                → EITHER arena, and the answer is ASKED
 *                 (`node_heap_arena_of` below). §4.6 has three constructors and they disagree —
 *                 `lxb_html_token_doctype_parse` allocates both from `mraw` while
 *                 `lxb_dom_document_type_interface_clone` and `lxb_dom_document_type_create` use `text` — so
 *                 there is no arena this list can name without being false for one of them. lexbor's own
 *                 destroy names `text` for all three; core/dom/document_type.h states why that mismatch is
 *                 fatal here and harmless upstream, and core/dom/document_type.c is the destroy that asks.
 *   - DocumentFragment, ShadowRoot: the struct and nothing else.
 * A kind with no arm CRASHES rather than answering true, because a silent skip IS the dangling pointer. */
/* WHICH of the agent's two arenas a pointer came out of, asked EXACTLY rather than assumed.
 * A caller has to ask when the ANSWER GENUINELY DIFFERS BY HOW THE THING WAS MADE, and §4.6's doctype ids are
 * the case that forced this to exist: `lxb_html_token_doctype_parse` allocates them from `mraw`, while
 * `lxb_dom_document_type_interface_clone` and `lxb_dom_document_type_create` both allocate them from `text`.
 * One doctype, three constructors, two arenas — so there is no arena a destroy can NAME, only one it can ask
 * for. This is not a defensive `if`: nothing is being skipped, and the answer NONE is a `DFAIL` at the caller.
 * It is the same chunk-list walk `dom_storage_owned_by` is built on and costs the same. */
typedef enum { NODE_ARENA_NONE = 0, NODE_ARENA_NODES, NODE_ARENA_TEXT } NodeArena;
NodeArena node_heap_arena_of(const void *p);

bool dom_storage_owned_by(const lxb_dom_document_t *doc, const lxb_dom_node_t *n);

#endif
