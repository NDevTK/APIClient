/* See node_interface.h. */
#include <stdlib.h>

#include <lexbor/html/html.h>
#include <lexbor/html/interface.h>
#include <lexbor/dom/interfaces/attr.h>   /* lxb_dom_attr_t — the list elem_release_attrs walks */
#include <lexbor/dom/interfaces/document_type.h>
#include <lexbor/dom/interfaces/element.h>

#include "check.h"
#include "core/dom/attr_list.h"     /* dom_attr_destroy — the ONE point an Attr's death converges on */
#include "core/dom/document_type.h"  /* document_type_destroy — §4.6's ids are not in the arena lexbor frees them into */
#include "core/dom/node.h"          /* node_template_content — the ONE spelling of where a template's markup is;
                                       node_wrap_forget / node_agent_runtime — the dying node hands its wrapper back */
#include "core/dom/node_heap.h"
#include "core/html/html_parse.h"   /* the ONE place a Document is parsed — the destroy asserts it was that one */
#include "core/dom/node_interface.h"
#include "solver/attr_shadow.h"     /* …and the taint slots keyed on the dying element go with it */

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
static lxb_dom_interface_t *dom_node_interface_free(lxb_dom_interface_t *intrfc)
{
    lxb_dom_node_t *node = intrfc;

    if (node == NULL)
        return NULL;
    /* §4.6's destroy is core/dom/document_type.c's and not lexbor's: lexbor allocates a doctype's two ids out
       of the NODE arena and frees them into the TEXT one, which with the agent-wide arenas of node_heap.h makes
       the two caches alias. document_type.h states the whole of it. */
    if (node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
        return document_type_destroy(lxb_dom_interface_document_type(intrfc));
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

/* HTML §4.12.3'S TEMPLATE CONTENTS ARE REACHED FROM NOWHERE ELSE, AND lexbor FREES THE FRAGMENT WITHOUT THEM.
 * "Each template element has an associated DocumentFragment object that is its template contents", and "The
 * template contents of a template element are not children of the element itself" — DOM §4.7 says the same from
 * its own side, "A DocumentFragment node has an associated host (null or an element in a DIFFERENT node tree)".
 * The OWNERSHIP of that fragment is an inert Document's: §4.12.3 gives "each Document not created by this
 * algorithm … a single Document to act as its proxy for owning the template contents of all its template
 * elements, so that they aren't in a browsing context and thus remain inert (e.g. scripts do not run)"
 * (https://html.spec.whatwg.org/multipage/scripting.html#the-template-element). That Document owns them in the
 * standard's sense and reaches them with nothing: the fragment is not a child of it either, so its own tree
 * stays EMPTY (core/dom/document.c says exactly that at `document_template_contents_owner`). So in the entire
 * heap the only pointer to a page's template markup is the ELEMENT's `content` field — and
 * `lxb_html_template_element_interface_destroy` destroys that fragment's STRUCT and returns, having freed
 * nothing that was in it. Upstream gets away with it because upstream nukes a per-document arena; this engine
 * has one heap for the agent (core/dom/node_heap.h) and frees node by node, so those nodes are now memory
 * nothing names and node_heap_detach's `ref_count == 0` is what said so.
 *
 * THE OWNER OF THE CONTENTS' LIFETIME IS THE ELEMENT, SO THIS IS WHERE THE FREE GOES — the ONE point every node
 * death converges on, exactly like the interface question above. Putting it in `dom_document_destroy` alone
 * would have covered the smallest of the paths that destroy a template: the per-flow delta's release of a node
 * a flow created (solver/dom_cow.c kind 4) is where `el.innerHTML = '<template>…'` templates die, because
 * `dom_cow_take_private` records every top-level node the fragment parse produced as that flow's creation and
 * the delta destroys it deep when it is discarded. `replaceChildren`, the sanitizer's removal walk and
 * `dom_cow_discard_private` are three more. A destroy that only some callers perform is the shape a leak comes
 * back through.
 *
 * IT IS A WORKLIST AND THE WORKLIST IS THE MODULE'S. `<template><template><template>` is ordinary markup, so
 * template nesting is depth the PAGE chooses; destroying a nested template from inside this function would
 * re-enter it through `lxb_dom_node_destroy_deep` and spend one C frame per level, which is the C stack this
 * engine does not have. So the queue is a module static and only the OUTERMOST entry drains it: a re-entrant
 * call pushes and returns, and the one drain loop below does the work at constant C depth. Nothing is bounded —
 * a queue that cannot grow is a CHECK, never a truncation, because the queue is the only remaining name for
 * those nodes.
 *
 * THE CHILDREN ARE DETACHED BEFORE THEY ARE QUEUED, because lexbor's template destructor frees the fragment
 * struct a few lines later and a queued node whose `parent` still named it would read freed memory at its own
 * `lxb_dom_node_remove`. It is `_wo_events` and not the removal: §4.2.3's removing steps belong to a DOM
 * operation observed by the page, and this is a teardown of a fragment that is ceasing to exist. */
static lxb_dom_node_t **g_tpl_queue;
static size_t g_tpl_n, g_tpl_cap;
static int g_tpl_draining;

static void tpl_queue_push(lxb_dom_node_t *n)
{
    /* THE NODE DOCUMENT MUST STILL HOLD THE AGENT'S HEAP, and this is the one site that can say so. §4.12.3's
       "establish the template contents" creates the fragment with the INERT owner as its node document, and its
       adopting steps keep it there — a Document whose own tree is empty, so it is destroyed without ever
       reaching these nodes and `node_heap_detach` NULLs its `mraw` on the way out. Freeing one of them
       afterwards hands `lexbor_mraw_free` a NULL arena. The inert owner has to outlive every document whose
       templates it owns; when this fires, that ordering is what to build, never a skipped free.
       IT IS REACHED TODAY ONLY THROUGH ADOPT, and that is a fidelity gap rather than a reprieve: lexbor's
       `lxb_html_template_element_interface_create` stamps the fragment with the document holding the ELEMENT,
       and core/dom/node.c's §4.5 step 3.4 is the only place this engine writes §4.12.3's answer. When the
       creation writes it too, every template's contents name the inert owner and this assertion covers them
       all rather than the adopted ones. */
    DCHECK(n->owner_document != NULL && n->owner_document->mraw != NULL,
           "a <template>'s contents are being queued for destruction with their NODE DOCUMENT already detached "
           "from the agent's DOM heap — HTML §4.12.3 gives the contents to an inert owner document whose own "
           "tree is empty, so that document was destroyed without ever reaching them; order the inert owner's "
           "destroy after every document whose templates it owns");
    if (g_tpl_n == g_tpl_cap) {
        size_t want = g_tpl_cap ? g_tpl_cap * 2 : 8;
        /* PLAIN realloc, NOT solver/reclaim.h's: this runs on the FREE path, and selling a flow to fund it
           would re-enter the delta release that is walking the very tree being torn down. */
        lxb_dom_node_t **v = realloc(g_tpl_queue, sizeof *v * want);

        CHECK(v != NULL, "a <template>'s contents could not be queued for destruction — this queue is the only "
                         "remaining name for those nodes, so a failed grow is the page's whole template tree "
                         "leaked and there is no bound to trade against it");
        g_tpl_queue = v;
        g_tpl_cap = want;
    }
    g_tpl_queue[g_tpl_n++] = n;
}

/* DOM §4.9'S ATTRIBUTES ARE THE SECOND STRUCTURE REACHED FROM A NODE THAT NO WALK OF CHILDREN SEES, AND FOR AN
 * HTML ELEMENT NOTHING IN LEXBOR'S TABLE FREES THEM. "An element has an attribute list, which is a list of
 * zero or more attributes" (https://dom.spec.whatwg.org/#concept-element-attribute), and an attribute is a NODE
 * whose parent is always null — solver/dom_cow.c says the same thing from §4.5's side, "an element's attributes
 * are not its descendants" — so `first_child` has never reached one, exactly as it has never reached a
 * template's contents.
 *
 * WHICH DESTRUCTOR RUNS IS WHAT DECIDES IT, and only one of them does the walk.
 * `lxb_dom_element_interface_destroy` iterates `first_attr` and destroys every Attr; it is reached for an
 * element lexbor built as a plain `lxb_dom_element_t` — the foreign-namespace arm of dom_node_interface_free
 * above, and lexbor's own unknown-tag-outside-HTML case. EVERY other element goes to
 * `lxb_html_interface_res_destructor[local_name][ns]` or `lxb_html_unknown_element_interface_destroy`, and
 * every one of those is the same single line, `lxb_dom_node_interface_destroy(node)`, which frees the element's
 * own struct and follows nothing. So on a page of HTML every attribute of every element was memory nothing
 * named: per attribute with a value, the Attr struct and the `lexbor_str_t` header out of the NODE arena and
 * its bytes out of the TEXT arena — three allocations in a 2:1 ratio, which is what node_heap.c's teardown
 * assert was counting when it reported 14 nodes against 7 text for a document holding seven attributes.
 * Upstream never sees it for the reason node_interface.h already gives about the other two defects in this
 * dispatch: upstream frees the whole arena and destroys no node on its own.
 *
 * SO THE LIST IS RELEASED HERE, at the one point every node death converges on, and the element is left holding
 * none — the arm that does reach `lxb_dom_element_interface_destroy` then walks an empty list, which is what
 * keeps this ONE statement rather than two that would have to agree about which elements each covers.
 * NO QUEUE, unlike the template contents: an Attr has no children, and `lxb_dom_attr_interface_destroy` is
 * lexbor's leaf free rather than a dispatch through `doc->destroy_interface`, so this cannot re-enter and costs
 * one C frame however long the list is.
 *
 * THE FREE IS `dom_attr_destroy` AND NOT LEXBOR'S, because an Attr is the ONE node kind whose death does not
 * reach this dispatcher, and attr_list.c is the point that plays this function's role for it: it hands the
 * attribute's wrapper back to the identity map and its slots back to the taint shadow before the struct goes.
 * Calling lexbor's leaf destructor here instead would be a SECOND place an Attr dies — and it was, which is
 * exactly how those two maps came to be left naming an address lexbor then handed out again. §4.9's `element`
 * is cleared for the whole list at once rather than per attribute through "remove an attribute": the list is
 * ceasing to exist along with the element that owns it, so there is no list left to unlink from and no removal
 * for the page to observe (the same reading that makes the template drain above `_wo_events`). It is unlinked
 * from the element BEFORE any struct is freed, so nothing can reach a freed attribute through it in between.
 *
 * AND THE ELEMENT'S OWN TAINT SLOTS GO WITH IT. The shadow (solver/attr_shadow.h) is keyed on an ELEMENT or on
 * an Attr, and an element's key covers both a content attribute's provenance and a DOM property's
 * (`textContent`, an input's value/checked/selected-files), so the element's death is what releases them. The
 * key is an ADDRESS out of the agent's node heap, so an entry left behind is inherited by whatever lexbor
 * allocates there next — a fresh element reading a destroyed one's taint, which is a wrong @S answer with
 * nothing to say so. */
static void elem_release_attrs(lxb_dom_node_t *node)
{
    lxb_dom_element_t *el;
    lxb_dom_attr_t *a, *next;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return;
    el = lxb_dom_interface_element(node);
    a = el->first_attr;
    el->first_attr = el->last_attr = NULL;
    el->attr_id = el->attr_class = NULL;
    for (; a != NULL; a = next) {
        next = a->next;
        DCHECK(a->owner == el,
               "an attribute in this element's list names a different element as its owner — §4.9's list and "
               "the attribute's `element` are two spellings of ONE edge (attr_list.c writes both in each of "
               "attach/detach/replace), so a disagreement here means the free below takes an attribute out of "
               "a list that is still holding it");
        a->owner = NULL;
        dom_attr_destroy(a);
    }
    attr_shadow_forget(node_agent_runtime(), el);
}

static lxb_dom_interface_t *dom_node_interface_destroy(lxb_dom_interface_t *intrfc)
{
    lxb_dom_node_t *node = intrfc, *content, *child;
    lxb_dom_interface_t *r;

    if (node == NULL)
        return NULL;
    content = node_template_content(node);
    if (content != NULL) {
        while ((child = content->first_child) != NULL) {
            lxb_dom_node_remove_wo_events(child);
            tpl_queue_push(child);
        }
        /* THE FRAGMENT ITSELF DIES HERE AND NOWHERE ELSE, so its wrapper goes back here. Its CHILDREN are
           queued above and each reaches this function on its own; the fragment does not. lexbor's template
           destructor frees its struct through `lxb_dom_document_fragment_interface_destroy`, a leaf free that
           never passes through `doc->destroy_interface` — and `t.content` is precisely the node a page holds a
           wrapper for. */
        node_wrap_forget(content);
    }
    elem_release_attrs(node);
    /* THE NODE HANDS ITS WRAPPER BACK BEFORE ITS BYTES GO, and this is the whole of where that happens.
     *
     * A NODE'S DEATH IS ONE EVENT AND IT IS THIS ONE. The map used to be swept by a walk in solver/dom_cow.c
     * that ran BEFORE four of the calls that destroy a subtree — and the sentence that stood here said so, as
     * if "the four paths" were a closed set. It never was: `dom_document_destroy` below, and every caller of
     * it in solve_html.c, solve.c, main.c and test_forced.c, destroyed a whole document's nodes with no sweep
     * anywhere near them. That was survivable while the arenas were a document's own, because the addresses
     * died with the document; since core/dom/node_heap.h made them the AGENT's, an address freed by one
     * document's destroy is handed straight back out to the live page document's next allocation, and a
     * surviving entry is then a wrapper whose opaque names another node's bytes.
     * A LIST OF CALLERS THAT MUST REMEMBER IS HOW THE FIFTH CALLER CAME TO EXIST, so there is no list: the
     * dispatcher is what lexbor reaches for every `lxb_dom_node_destroy` and every `_destroy_deep`, whoever
     * calls them, and the forget is one line inside it. Nothing has to be added anywhere for a new caller to
     * be covered, which is the only property that makes this statement true rather than currently true. */
    node_wrap_forget(node);
    r = dom_node_interface_free(intrfc);
    if (!g_tpl_draining) {
        g_tpl_draining = 1;
        /* Every entry is an independent DETACHED subtree, so the order among them is free; LIFO keeps the queue
           shallow, and each `destroy_deep` re-enters the function above, which pushes rather than descends. */
        while (g_tpl_n > 0)
            lxb_dom_node_destroy_deep(g_tpl_queue[--g_tpl_n]);
        g_tpl_draining = 0;
        /* The buffer goes back with the work: a queue kept between destroys is an allocation the agent's own
           teardown has nobody to name. */
        free(g_tpl_queue);
        g_tpl_queue = NULL;
        g_tpl_cap = 0;
    }
    return r;
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
    /* AND ITS PARSES' TOKEN ATTRIBUTE VALUES HAVE AN OWNER — see core/html/html_parse.h. It goes on the same
       resolved document as the dispatcher above and for the identical reason: `lxb_dom_element_attr_append`
       reads `attr_mutation` off `lxb_dom_interface_node(element)->owner_document`, so the field the DOM reads
       is the owner's and installing on an inherited struct would write the one pointer nothing reads. That is
       also what covers §13.4's fragment parse, whose temporary document stamps every element and Attr it makes
       with this one while carrying lexbor's own HTML attribute steps in its own field —
       `lxb_html_document_mutation_init` installs those on every HTML document at creation, which is also what
       this install composes over rather than replaces. */
    html_parse_own_token_values(doc);
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
    /* AND ITS PARSE WENT THROUGH core/html/html_parse.h, asked here because this is the one line every
       document reaches and because a list of parse call sites that must remember is how the next one comes to
       be forgotten. `lxb_html_document_parse` CREATES the parser it uses, so a document that reached lexbor's
       entry directly is holding a tokenizer this engine never took ownership of — and every attribute value
       that parse read and tree construction did not adopt is left in the agent's text arena with nothing
       naming it, which node_heap_detach then reports as a count with no cause. */
    DCHECK(html_parse_owns_tokens_of(doc),
           "a document is being destroyed whose parse did not go through core/html/html_parse.h — it holds an "
           "HTML parser this engine did not build, so its tokens' attribute values had no owner and every "
           "duplicate attribute, every re-attributed `<html>`/`<body>` attribute and every doctype id in that "
           "markup is one text allocation the agent's heap can never give back");
    /* `lxb_dom_node_destroy_deep` DETACHES each node before it frees it, so the document's child list drains
       as the loop runs; it is iterative, so the depth of the page's markup costs no C stack. */
    while ((child = doc->node.first_child) != NULL)
        lxb_dom_node_destroy_deep(child);
    /* THE DOCUMENT'S OWN NODE IS THE ONE NODE THIS FUNCTION FREES WITHOUT THE DISPATCHER, so it is the one
       node whose wrapper this function has to hand back itself. `lxb_html_document_destroy` frees the struct
       directly, and `document` is a node a page wraps on its very first statement. */
    node_wrap_forget(&doc->node);
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
