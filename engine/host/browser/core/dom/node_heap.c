/* See node_heap.h. */
#include <stdio.h>    /* snprintf — the teardown assert formats the two live counts into its own message */
#include <stddef.h>
#include <stdint.h>

#include <lexbor/core/mem.h>
#include <lexbor/core/mraw.h>
#include <lexbor/core/str.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document_type.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/processing_instruction.h>

#include "check.h"
#include "core/dom/node_heap.h"

/* THE AGENT IS THE INSTANCE, WHICH IS WHY THIS IS A MODULE STATIC AND NOT A LOOKUP. CLAUDE.md's rule against a
   module static answering one fact for many agents is exactly what decides this one: a WASM instance holds ONE
   origin-keyed agent cluster, so the process HAS one heap, and asking a document for it would be asking N
   places for a fact there is one of. A DIFFERENT-ORIGIN document is a different INSTANCE and gets its own
   process image and its own pair of these — which is the same boundary the cross-instance transport crosses
   with TEXT, and the reason a live node pointer may not cross it.
   The chunk sizes are lexbor's own for a document (dom/interfaces/document.c's `lxb_dom_document_init`), kept
   identical so that swapping the arenas changes WHERE the bytes come from and not how they are shaped. */
static lexbor_mraw_t *g_nodes;
static lexbor_mraw_t *g_text;
static size_t         g_documents;

#define NODE_HEAP_NODE_CHUNK  (4096 * 8)
#define NODE_HEAP_TEXT_CHUNK  (4096 * 12)

static lexbor_mraw_t *node_heap_arena(size_t chunk)
{
    lexbor_mraw_t *m = lexbor_mraw_create();

    CHECK(m != NULL, "node-heap-oom: the agent's DOM heap could not be created — every node of every document "
                     "in this instance is allocated out of it");
    CHECK(lexbor_mraw_init(m, chunk) == LXB_STATUS_OK,
          "node-heap-oom: the agent's DOM heap could not be initialised");
    return m;
}

void node_heap_attach(lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "no document was attached to the agent's DOM heap");
    DCHECK(doc->mraw != NULL && doc->text != NULL,
           "a document reached the agent's DOM heap with no arenas of its own — lxb_dom_document_init creates "
           "both for an ORIGINAL document, and an INHERITED one already shares its owner's and must never be "
           "attached a second time");
    /* THE ATTACH IS BEFORE THE FIRST NODE, and this is what says so. The two arenas below are about to be
       destroyed; anything already allocated out of them is freed here and read afterwards. lexbor's own
       `ref_count` is the live-allocation count, so a fresh document answers 0 and no other state is needed. */
    DCHECK(doc->mraw->ref_count == 0 && doc->text->ref_count == 0,
           "a document was attached to the agent's DOM heap after something had already allocated out of the "
           "private arenas lxb_dom_document_init made for it — those bytes are freed by this call, so the "
           "attach belongs at dom_document_create, before a single node exists");
    if (g_nodes == NULL) {
        DCHECK(g_documents == 0 && g_text == NULL,
               "the agent's DOM heap is half gone — the node arena and the text arena are created together and "
               "destroyed together with the last document, so one without the other is a torn teardown");
        g_nodes = node_heap_arena(NODE_HEAP_NODE_CHUNK);
        g_text  = node_heap_arena(NODE_HEAP_TEXT_CHUNK);
    }
    lexbor_mraw_destroy(doc->mraw, true);
    lexbor_mraw_destroy(doc->text, true);
    doc->mraw = g_nodes;
    doc->text = g_text;
    g_documents++;
}

void node_heap_detach(lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "no document gave up its claim on the agent's DOM heap");
    DCHECK(doc->mraw == g_nodes && doc->text == g_text,
           "a document is giving up arenas that are not the agent's — it was not built by dom_document_create, "
           "and lexbor's destroy would then free memory every other document in this instance is still "
           "allocating its nodes out of");
    DCHECK(g_documents > 0, "more documents gave up the agent's DOM heap than ever attached to it");
    doc->mraw = NULL;
    doc->text = NULL;
    if (--g_documents == 0) {
        /* THE HEAP IS EMPTY WHEN THE LAST DOCUMENT IS GONE, and lexbor's `ref_count` states it exactly: it is
           incremented by every alloc and decremented by every free, so a non-zero count here is memory no
           document, no tree and no wrapper names any more.
           TWO STRUCTURES USED TO BE NAMED HERE and are not any more, and they were one defect wearing two
           shapes: HTML §4.12.3's TEMPLATE CONTENTS and DOM §4.9's ATTRIBUTE LIST are each reached from an
           element and from nowhere in any tree, and lexbor's per-tag HTML destructors free the element's own
           struct and follow neither. Both are freed with the element by core/dom/node_interface.c's destroy
           dispatcher, which is the one point every node death converges on, so the document tree walk, the
           per-flow delta's kind-4 release and every other destroy carry them. What is left are the trees a walk
           of CHILDREN still cannot reach and no C pointer names:
             - A SHADOW ROOT. DOM §4.8's shadow root is a second tree exactly as a template's contents are, and
               the element→shadow-root edge is on the host's WRAPPER (core/dom/shadow_root.c writes it as a slot
               on the element's JS object, which is what `shadow_root_of_element` peeks), so no C walk can see
               it and only the delta's kind-4 entry frees one. A shadow root attached while capture was off has
               no owner at all, and node_interface.c's dispatcher cannot become that owner: it holds no realm,
               so the edge it would have to follow is one it cannot read.
             - A node created while capture was off and then DETACHED. Kind 4 frees what a FLOW created; a
               creation outside one is baseline and its owner is the document it was made in, which frees its
               TREE and nothing that left it.
             - AND AN ORDER, not a leak: every delta must be released BEFORE the last document detaches, since a
               kind-4 node freed after this line is freed out of an arena that is gone. */
        /* THE TWO COUNTS ARE THE DIAGNOSIS AND THE PROSE ALONE IS NOT. This named three causes and printed
           neither number, so the reader standing at the teardown could not tell which of the three they were
           looking at — and the three want opposite fixes. §Testing's rule that a message naming several causes
           and distinguishing none of them is a report about nothing applies to a DCHECK exactly as it applied
           to the kill that said "segfault/abort/timeout": the count is what separates them.
           AND IT IS THE RATIO THAT SEPARATES THEM, WHICH THE FIRST VERSION OF THIS MESSAGE DID NOT SAY. It led
           with the shadow root for every small count, and the first count it ever printed — 14 nodes against 7
           text — was not one: it was seven attributes, because an attribute with a value is THREE allocations
           (the Attr and its `lexbor_str_t` header here, its bytes there) and no HTML element destructor freed
           any of them. A reader sent to `shadow_root.c` by a message that leads with the arm it cannot be is
           the stale-DFAIL failure with a number attached, so the arithmetic goes first and the trees after it.
           §Testing states the general form of this at the allocation that OOMs: "the reader of a `@WHY` is
           standing at the allocation, and 'OOM' alone tells them nothing about realms". */
#if APICLIENT_DEV
        {
            /* THE BUFFER IS THE MESSAGE'S SIZE AND NOT A ROUND NUMBER. At 1024 the format expanded to at
               least 1305 and `snprintf` truncated it mid-word — the ORDER arm, the last third and the one a
               large count needs, never reached the reader, and the build's own `-Wno-format-truncation` is why
               no gate said so. A diagnosis that is cut off is §Testing's report about nothing with the
               explanation attached. The literal below is a little under 3000 characters and each `%zu` can
               expand to 20, so this holds it with the two counts at their widest and an arm's worth of room
               for the next cause that has to be named here. */
            char why[4096];
            snprintf(why, sizeof why,
                     "the agent's DOM heap still holds %zu node allocation(s) and %zu text allocation(s) after "
                     "the last document that could name them was destroyed. READ THE RATIO FIRST, because the "
                     "two arenas count different things and the list of what each kind owns is node_heap.h's: "
                     "an ATTRIBUTE with a value is three allocations, two here and one there, so nodes at "
                     "TWICE text is an attribute list nothing freed; a TEXT, COMMENT or CDATASection is one of "
                     "each, so nodes at ROUGHLY text is character data; text at ZERO with nodes non-zero is "
                     "element or fragment structs alone. THEN THE MAGNITUDE. A HANDFUL is a second structure "
                     "reached from an element and from nowhere in any tree, which no walk of children can see "
                     "— DOM §4.8's shadow root is the one left, its edge from the host being a slot on the "
                     "host's WRAPPER, so only a per-flow delta's kind-4 entry ever frees one (§4.9's "
                     "attributes and §4.12.3's template contents are the same shape and are freed by "
                     "core/dom/node_interface.c's dispatcher). HUNDREDS, in proportion, is the ORDER instead: "
                     "a delta released after this line rather than before it, leaving a whole flow's creations "
                     "live (and every one of them would then be freed out of an arena that is gone). A node "
                     "created while capture was off and then detached is the remaining shape and looks like "
                     "the handful. AND A HUGE COUNT IS NOT A LEAK AT ALL — `ref_count` is UNSIGNED and "
                     "`lexbor_mraw_free` decrements it while validating nothing, so a count near SIZE_MAX is "
                     "that many frees TOO MANY: read it as a negative and the magnitude is the number of "
                     "double frees. It is the more serious half, because the same call also inserted a pointer "
                     "this arena never handed out into this arena's size-keyed free cache, so the two arenas "
                     "now ALIAS and the next allocation of that size gets memory the other one still owns. "
                     "core/dom/document_type.h is the worked example: lexbor allocates a doctype's two ids "
                     "from `mraw` and frees them into `text`, which is exactly nodes at +2 and text at -2. "
                     "FINALLY, TEXT AT +N WITH NODES UNCHANGED IS AN ATTRIBUTE VALUE WHOSE OWNERSHIP NEVER "
                     "MOVED, and no other arm produces that shape. A token's attribute values come out of "
                     "this arena and no lexbor destructor frees them; core/html/html_parse.h owns them "
                     "instead, releasing at token-done whatever the DOM did not take and learning what the "
                     "DOM took from the document's `node_cb->insert`, which `lxb_dom_element_attr_append` "
                     "fires with the adopted pointer already in `attr->value->data`. So this count means one "
                     "of the two halves did not run for some parse: a document whose `node_cb` is not that "
                     "one (the token-done wrapper asserts it per token, so read that @WHY first), or a parse "
                     "whose tokenizer never got the wrapper at all (`html_parse_owns_tokens_of`, asserted at "
                     "dom_document_destroy). A value adopted through some path that is NOT "
                     "`lxb_dom_element_attr_append` would show up as the OPPOSITE — a double free, read the "
                     "huge-count arm above.",
                     (size_t)g_nodes->ref_count, (size_t)g_text->ref_count);
            DCHECK(g_nodes->ref_count == 0 && g_text->ref_count == 0, why);
        }
#endif
        g_nodes = lexbor_mraw_destroy(g_nodes, true);
        g_text  = lexbor_mraw_destroy(g_text, true);
    }
}

/* Is `p` one of `mraw`'s chunks? A chunk is a contiguous `[data, data + size)` and `mraw->mem` links them
   `chunk` → `prev` → … → `chunk_first`, so this is exact for every allocation the arena has ever handed out,
   live or cached. It answers FALSE for a NULL arena rather than asserting: a detached document has none, and
   asking the predicate about one is how that is caught. */
static bool mraw_owns(const lexbor_mraw_t *mraw, const void *p)
{
    const lexbor_mem_chunk_t *c;
    const uint8_t *q = (const uint8_t *)p;

    if (mraw == NULL || mraw->mem == NULL || p == NULL)
        return false;
    for (c = mraw->mem->chunk; c != NULL; c = c->prev)
        if (c->data != NULL && q >= c->data && q < c->data + c->size)
            return true;
    return false;
}

/* A `lexbor_str_t` whose `data` is NULL owns nothing — `lexbor_str_destroy` is the same statement from the
   other side, and lexbor leaves the field NULL on a string nothing has written. */
static bool str_owned(const lexbor_mraw_t *text, const lexbor_str_t *s)
{
    return s->data == NULL || mraw_owns(text, s->data);
}

/* THE SAME QUESTION WITH NO ARENA NAMED, for the one shape whose ANSWER DIFFERS BY HOW IT WAS MADE. DOM §4.6's
   doctype has three constructors and they disagree: `lxb_html_token_doctype_parse` allocates its two ids from
   `mraw` while `lxb_dom_document_type_interface_clone` and `lxb_dom_document_type_create` both use `text`. So
   naming either arena answers FALSE for whichever constructor did not use it — `text` was false for every
   parsed doctype, and `mraw` (which replaced it) is false for every doctype `createDocumentType` or a §4.4
   clone produced. Both arenas are the AGENT's, so what this predicate is actually asking is whether the bytes
   are in the agent's heap at all, and core/dom/document_type.c's destroy asks the identical way. */
static bool str_in_agent_heap(const lexbor_str_t *s)
{
    return s->data == NULL || node_heap_arena_of(s->data) != NODE_ARENA_NONE;
}

NodeArena node_heap_arena_of(const void *p)
{
    bool in_nodes = mraw_owns(g_nodes, p), in_text = mraw_owns(g_text, p);

    /* THE TWO ARENAS ARE DISJOINT REGIONS, so a pointer in both is the aliasing this file's teardown assertion
       exists to catch, seen one allocation earlier and from the other side. */
    DCHECK(!(in_nodes && in_text),
           "a pointer is inside a chunk of BOTH of the agent's DOM arenas — they hand out disjoint memory, so "
           "one of them has taken a free of a pointer the other allocated and their caches now alias");
    return in_nodes ? NODE_ARENA_NODES : in_text ? NODE_ARENA_TEXT : NODE_ARENA_NONE;
}

bool dom_storage_owned_by(const lxb_dom_document_t *doc, const lxb_dom_node_t *n)
{
    DCHECK(doc != NULL && n != NULL, "the node-storage ownership invariant was asked of nothing");

    /* THE STRUCT FIRST, because it is the one every kind has and the one that cannot move. */
    if (!mraw_owns(doc->mraw, n))
        return false;

    switch (n->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        const lxb_dom_element_t *el = (const lxb_dom_element_t *)n;
        /* `is_value` is a `lexbor_str_t *` out of `doc->mraw` with its bytes out of `doc->text`, and it is the
           one thing on this list that NOTHING frees: core/dom/node_interface.c's `elem_release_attrs` walks
           the attribute list on the way to the element's own destroy and never touches it, and neither does
           any of the per-tag destructors that run after it. Nothing in this engine sets one either —
           core/dom/node.c's §4.4 clone asserts the same absence at the one site that would have dropped it —
           so the honest statement is that it is absent, not that it is owned. The day `is` becomes real this
           fires here as well as there, and both sites are then the list of what has to learn about it. */
        DCHECK(el->is_value == NULL,
               "an element carries an `is` value: it is a lexbor_str_t in the node arena with its bytes in the "
               "text arena, no destructor frees either, and DOM §4.5's adopt has no rule for it — give this "
               "arm its two pointers and give core/dom/node_interface.c's elem_release_attrs its free, beside "
               "the attribute list it already releases");
        return el->is_value == NULL;
    }
    case LXB_DOM_NODE_TYPE_ATTRIBUTE: {
        const lxb_dom_attr_t *a = (const lxb_dom_attr_t *)n;
        /* §4.9.2's value is a SEPARATE allocation from the Attr — the header in the node arena, the bytes in
           the text arena — which is why an attribute is two questions and not one. */
        if (a->value == NULL)
            return true;
        return mraw_owns(doc->mraw, a->value) && str_owned(doc->text, a->value);
    }
    case LXB_DOM_NODE_TYPE_TEXT:
    case LXB_DOM_NODE_TYPE_CDATA_SECTION:
    case LXB_DOM_NODE_TYPE_COMMENT: {
        const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
        return str_owned(doc->text, &cd->data);
    }
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION: {
        const lxb_dom_processing_instruction_t *pi = (const lxb_dom_processing_instruction_t *)n;
        /* §4.7's `target` is its own string BESIDE the character data it inherits, and it is the second half
           of what `lxb_dom_processing_instruction_interface_destroy` frees. */
        return str_owned(doc->text, &pi->char_data.data) && str_owned(doc->text, &pi->target);
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
        const lxb_dom_document_type_t *dt = (const lxb_dom_document_type_t *)n;
        /* §4.6's publicId and systemId are STRINGS here, not name ids — the doctype's `name` is the `attrs` id
           name_intern.h moves, and these two are the bytes this file moves nowhere because the arena is one.
           THEY ARE THE ONE SHAPE WITH NO ARENA TO NAME, and this line has now named the wrong one twice: it
           said `text` because lexbor's own DESTROY says `text`, which is false for every PARSED doctype, and
           then `mraw` because the parser allocates there, which is false for every doctype
           `createDocumentType` or a §4.4 clone produced — so `document.implementation.createDocumentType(…)`
           followed by an adopt fired this assertion on a document that was perfectly well formed. Three
           constructors, two arenas, and the question this predicate is really asking is whether the bytes are
           in the agent's heap at all. core/dom/document_type.h holds the full statement and
           core/dom/document_type.c is the destroy that asks the same way. */
        return str_in_agent_heap(&dt->public_id) && str_in_agent_heap(&dt->system_id);
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
    case LXB_DOM_NODE_TYPE_SHADOW_ROOT:
        return true;                       /* the struct, and nothing else — both creators calloc and stop */
    case LXB_DOM_NODE_TYPE_DOCUMENT:
        DFAIL("a Document's storage ownership was asked against another document — a document IS its own node "
              "document, its struct is `lexbor_calloc`'d rather than taken from any arena, and §4.5's "
              "adoptNode throws NotSupportedError rather than reaching one");
        return false;
    default:
        DFAIL("a node kind with no arm above was asked whether its storage belongs to this document — the list "
              "is derived from lexbor's per-interface destructors, so a kind that reaches here either owns "
              "bytes nothing frees or is a historical type (ENTITY, ENTITY_REFERENCE, NOTATION) this engine "
              "never creates; give it its own case rather than letting the answer be a guess");
        return false;
    }
}
