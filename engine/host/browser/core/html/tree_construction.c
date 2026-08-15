/* HTML §13.2.4's PARSE STATE, MID-PARSE, COPIED.
 *
 * WHAT THIS IS FOR. A flow parked inside `el.innerHTML = markup` is parked inside §13.4's fragment parsing
 * algorithm: core/dom/element.c's machine feeds the parser one byte per step and rests between bytes, and at
 * every one of those rests the scheduler may fork the flow — a concolic branch in a sibling, RAM pressure, a
 * cold-tier eviction, a cross-session resume. A fork hands each arm its OWN state, and the piece of that state
 * lexbor owns is an lxb_html_tree_t standing at an insertion mode with a stack of open elements under it. Two
 * arms sharing one of those is two parses writing one stack, which is precisely the corruption per-flow
 * isolation exists to prevent, so element.c's frag_unforkable ABORTS the fork rather than allow it. This file
 * is the first of the two halves that sentence names, and it is written first for the reason it gives: this
 * half needs nothing lexbor does not already expose.
 *
 * WHY A COPY AND NOT A REPLAY. The machine owns the markup and the offset into it, so a sibling could rebuild
 * its state by re-feeding bytes [0, off) to a fresh parser, and that is deterministic. It is also the exact
 * drive-to-completion the one-byte-per-step machine was built to remove, moved to the fork: a fork taken at
 * byte 100000 of a page's markup would re-tokenise 100000 bytes inside one opcode, unpreemptible. A fork is
 * already O(state) — JSStepVisit's `buf`, `slots`, `props` and `array` each copy their whole allocation — and
 * O(state) is what this is.
 *
 * WHY NO §4.5 ADOPT, AND WHY NO NAME IS RE-INTERNED. §13.4's temporary document is built with
 * lxb_html_document_interface_create(document), and lxb_dom_document_init with a non-NULL owner INHERITS mraw,
 * text, tags, attrs, ns, prefix, css and parser from it and sets node.owner_document to it. So the parse's
 * nodes are allocated out of the REAL document's arena, are stamped with the REAL document, and intern their
 * tag and attribute names in the REAL document's hashes. The copy's temporary document is created against that
 * SAME real document, which is what makes the copy identical rather than merely equal: lxb_dom_node_interface_copy
 * takes its `dst->owner_document == src->owner_document` path and assigns `local_name`, `ns` and `prefix`
 * verbatim, so a copied element carries the SAME interned ids and the placement that follows the parse still
 * needs no adopt. Build the copy's temporary document against anything else and every name is re-interned into
 * another hash — silently, and correctly-looking. Asserted per node, at the copy.
 *
 * WHAT THE MAP IS AND HOW LONG IT LIVES. A node->node map, keyed on the source node's address, built by the
 * subtree walk and DESTROYED when this call returns. It exists only so the tree builder's arrays can be moved
 * onto the copy; nothing about it has to park, which is why it is a stack-lifetime artifact and not a field of
 * anything. Every entry is a pointer into the real document's arena on both sides.
 *
 * WHAT IS NOT HERE: §13.2.5's tokenizer. It is the caller's `tkz` argument, and element.c's frag_unforkable
 * states what building it costs and why it buys the FORK and not the cold tier. */
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/core/avl.h>
#include <lexbor/core/array.h>
#include <lexbor/core/array_obj.h>
#include <lexbor/core/str.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/html/interface.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/tree.h>
#include <lexbor/html/tree/active_formatting.h>
#include <lexbor/html/tree/template_insertion.h>

#include "check.h"
#include "core/dom/node.h"
#include "core/html/tree_construction.h"
#include "solver/dom_cow.h"

/* THE NODE->NODE MAP. lexbor's own AVL, which is keyed on a `size_t` and carries a `void *` — a pointer map
   with no structure to invent, in the pinned tree, allocated out of a dobject it destroys with itself. */
typedef struct {
    lexbor_avl_t      *avl;
    lexbor_avl_node_t *root;
} NodeMap;

static void map_begin(NodeMap *m)
{
    m->avl = lexbor_avl_create();
    m->root = NULL;
    CHECK(m->avl != NULL && lexbor_avl_init(m->avl, 128, sizeof(lexbor_avl_node_t)) == LXB_STATUS_OK,
          "the fragment-parse copy could not create its node map");
}

static void map_end(NodeMap *m)
{
    m->avl = lexbor_avl_destroy(m->avl, true);
    m->root = NULL;
}

static void map_put(NodeMap *m, const lxb_dom_node_t *src, lxb_dom_node_t *dst)
{
    DCHECK(src != NULL && dst != NULL, "the fragment-parse copy's node map was given half of a pair");
    /* A SECOND ENTRY FOR ONE SOURCE NODE IS A WALK THAT REACHED IT TWICE, and lexbor's insert would answer the
       later one — so the tree builder's arrays would be remapped onto a copy nothing is linked to. The walk
       reaches a `<template>`'s content through the element and every other node through child links exactly
       once, which is the invariant this states. */
    DCHECK(lexbor_avl_search(m->avl, m->root, (size_t)(uintptr_t)src) == NULL,
           "the fragment-parse copy reached one source node twice — the second copy would be the one the tree "
           "builder's arrays are remapped onto, and nothing would be linked to it");
    CHECK(lexbor_avl_insert(m->avl, &m->root, (size_t)(uintptr_t)src, dst) != NULL,
          "the fragment-parse copy could not record a node in its map");
}

static lxb_dom_node_t *map_get(const NodeMap *m, const lxb_dom_node_t *src)
{
    lexbor_avl_node_t *e;
    if (src == NULL) return NULL;
    e = lexbor_avl_search(m->avl, m->root, (size_t)(uintptr_t)src);
    return e != NULL ? (lxb_dom_node_t *)e->value : NULL;
}

/* The tree a copied node belongs to — the copy's temporary document node, or a `<template>`'s content fragment.
   Both are roots with no parent, which is what makes this the same question dom_cow.h's private-tree
   declaration asks. No C recursion: the depth here is the page's markup. */
static lxb_dom_node_t *copy_private_root(lxb_dom_node_t *n)
{
    while (n->parent != NULL) n = n->parent;
    return n;
}

/* Is `n` the content fragment of a `<template>` — the one tree a parse builds that is reached other than
   through child links. A shadow root is a document fragment too in DOM's terms, which is why the test is the
   round trip through the host rather than the node type alone: a parse produces no shadow roots (§13.2.6.4.4's
   declarative conversion runs at the parse BOUNDARY, over the finished fragment), so one appearing here is a
   tree this walk was never given and the caller below says so. */
static bool is_template_content(const lxb_dom_node_t *n)
{
    const lxb_dom_element_t *host;
    if (n->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) return false;
    host = lxb_dom_interface_document_fragment(n)->host;
    return host != NULL && node_template_content(lxb_dom_interface_node(host)) == n;
}

/* ONE NODE OF THE PARTIAL TREE, COPIED — lexbor's own `clone_interface` for this document, which is the code
   the tree builder itself uses to make a node, so attributes, namespaces and every per-interface field are
   copied by it and not by a second answer written here. */
static lxb_dom_node_t *copy_one(lxb_html_document_t *doc, const lxb_dom_node_t *src)
{
    lxb_dom_node_t *c;

    /* NOTHING HAS SEEN THIS TREE. The parse runs no page code (element.c's machine states that invariant and
       §13.4's Inert scripting mode is what holds it), so no node of it can carry a JS wrapper — and if one did,
       the copy would need an identity answer for it rather than a fresh node. */
    DCHECK(JS_IsUndefined(node_wrap_peek(src)),
           "a node of a partial fragment parse already has a JS wrapper — the parse is supposed to be a tree "
           "nothing has ever seen, so a fork would have to decide which arm's node that wrapper names");
    c = lxb_html_interface_clone(lxb_dom_interface_document(doc), src);
    CHECK(c != NULL, "the fragment-parse copy could not copy a node of the partial tree");
    /* THE COPY IS IN THE SAME DOCUMENT AS THE ORIGINAL, which is the whole of why no name is re-interned and
       why the placement after the parse still needs no §4.5 adopt. It holds because the copy's temporary
       document was created against the same real document; the moment it is not,
       lxb_dom_node_interface_copy takes its other path and every tag and attribute name lands in another
       hash. */
    DCHECK(c->owner_document == src->owner_document,
           "a copied fragment-parse node belongs to a different document than the node it was copied from — "
           "its interned tag and attribute ids are then meaningless against the document the placement inserts "
           "it into");
    return c;
}

/* WHERE THE WALK GOES AFTER `n`'s SUBTREE IS FINISHED. Iterative, and it climbs by `parent` and by a content
   fragment's `host` rather than carrying a level stack: those two links are the only way into or out of a level,
   and the tree already holds both. `croot` is updated at the one transition that changes it — leaving a
   `<template>`'s content for the template element's OWN children, which §4.10 keeps as two separate child
   lists. */
static lxb_dom_node_t *walk_next(lxb_dom_node_t *n, const lxb_dom_node_t *top, const NodeMap *map,
                                 lxb_dom_node_t **croot)
{
    for (;;) {
        lxb_dom_node_t *up, *host, *host_copy;

        if (n->next != NULL) return n->next;
        up = n->parent;
        DCHECK(up != NULL,
               "the fragment-parse copy walked out of the tree it was given — it climbs to the temporary "
               "document it started at, and a node with no parent below that is a second tree this walk was "
               "never handed");
        if (is_template_content(up)) {
            host = lxb_dom_interface_node(lxb_dom_interface_document_fragment(up)->host);
            host_copy = map_get(map, host);
            DCHECK(host_copy != NULL,
                   "the fragment-parse copy left a <template>'s content and its host element has no copy — the "
                   "host is copied before its content is walked, so the map cannot be missing it");
            *croot = copy_private_root(host_copy);
            if (host->first_child != NULL) return host->first_child;
            n = host;
            continue;
        }
        if (up == top) return NULL;
        DCHECK(map_get(map, up) != NULL,
               "the fragment-parse copy climbed into a node it never copied — the only trees it enters are the "
               "temporary document's and a <template>'s content, and it copies every node of both");
        n = up;
    }
}

/* THE PARTIAL TREE, DEEP-COPIED, AND THE MAP THAT RECORDS IT. `src_top` is the source parse's temporary
   document node and `dst_top` the copy's; the walk copies every descendant of the first into the second, in
   tree order, and enters both in the map so `map_get(node->parent)` is the insertion parent uniformly.
   Every insert is DECLARED PRIVATE (dom_cow.h): the copy is a tree no other flow can reach, so capturing it
   would put a whole parse into the running flow's delta, which the delta exists not to hold. */
static void copy_subtree(lxb_html_document_t *doc, lxb_dom_node_t *src_top, lxb_dom_node_t *dst_top, NodeMap *map)
{
    lxb_dom_node_t *src = src_top->first_child;
    lxb_dom_node_t *croot = dst_top;

    map_put(map, src_top, dst_top);
    while (src != NULL) {
        lxb_dom_node_t *parent = map_get(map, src->parent);
        lxb_dom_node_t *copy, *content, *ccontent;

        DCHECK(parent != NULL,
               "the fragment-parse copy reached a node whose parent it has not copied — the walk is pre-order "
               "over both trees, so a parent is always in the map before its children");
        copy = copy_one(doc, src);
        dom_cow_insert_private(croot, parent, copy);
        map_put(map, src, copy);

        content  = node_template_content(src);
        ccontent = node_template_content(copy);
        if (content != NULL) {
            /* §4.12.3: a `<template>`'s markup is NOT under the element, it is in a separate fragment reached
               through it — and lexbor's clone_interface gives the copy its own EMPTY one, because the template
               interface's constructor makes it. Both sides therefore have somewhere to go, and the fragment
               goes in the map for the same reason every element does: it is the insertion parent its children
               will be looked up by. */
            DCHECK(ccontent != NULL,
                   "a copied <template> has no content fragment — lexbor's clone_interface built something "
                   "other than a template interface for a node whose tag is template");
            map_put(map, content, ccontent);
            if (content->first_child != NULL) {
                croot = ccontent;
                src = content->first_child;
                continue;
            }
        } else {
            DCHECK(ccontent == NULL,
                   "the copy of a non-template node has a template content fragment — the two sides of this "
                   "walk have stopped being the same kind of node");
        }
        if (src->first_child != NULL) { src = src->first_child; continue; }
        src = walk_next(src, src_top, map, &croot);
    }
}

/* §13.4 step 3's CONTEXT ELEMENT and step 5's synthetic FORM — the two nodes a fragment parse creates that are
   in NO tree, so the subtree walk above never reaches them and each needs its own copy. Both are destroyed by
   lxb_html_parse_fragment_chunk_destroy, so a sibling arm sharing them would have them freed under it by
   whichever arm finished first. They go in the map so the field remaps below are one uniform lookup. */
static lxb_dom_node_t *copy_detached(lxb_html_document_t *doc, lxb_dom_node_t *src, NodeMap *map)
{
    lxb_dom_node_t *c;

    DCHECK(src->parent == NULL && src->first_child == NULL,
           "§13.4's context element and its synthetic form are created by the fragment parsing algorithm and "
           "never inserted anywhere — one that has a parent or children is a node the parse put in a tree, and "
           "the subtree walk owns those");
    c = copy_one(doc, src);
    map_put(map, src, c);
    return c;
}

void html_tree_construction_copy(const lxb_html_tree_t *src, lxb_dom_node_t *src_root, lxb_dom_node_t *src_form,
                                 lxb_html_tokenizer_t *tkz, HtmlInputRebase rebase, void *rebase_ctx,
                                 HtmlTreeConstructionCopy *out)
{
    lxb_dom_document_t *real, *src_doc, *copy_doc;
    lxb_html_tree_t *copy;
    NodeMap map;
    size_t i, n;

    DCHECK(src != NULL && src_root != NULL && tkz != NULL && out != NULL,
           "the fragment-parse copy was asked for with a piece of its subject missing");
    DCHECK(rebase != NULL,
           "the fragment-parse copy was called with no input rebase — every parse error records the token "
           "EXTENT that raised it, and those name bytes in §13.2.5's incoming buffer, so a copy that took them "
           "verbatim would hand the sibling flow two pointers into the original arm's input");
    DCHECK(src->document != NULL,
           "an lxb_html_tree with no document was asked to be copied — §13.4 attaches its temporary document "
           "before the first byte, so this tree is one no fragment parse started");
    DCHECK(src->fragment != NULL,
           "a tree-construction copy was asked for a parse that is NOT a fragment parse. A document parse "
           "builds into the REAL document, whose tree is shared baseline state the per-flow COW delta isolates "
           "— copying it would be a second document, not a second arm. Build that as its own operation if a "
           "document parse ever has to fork");

    src_doc = lxb_dom_interface_document(src->document);
    DCHECK(!lxb_html_document_is_original(src->document),
           "§13.4's temporary document is created against the real one and reports itself as not original; a "
           "tree whose document IS original is parsing into a real document, which is the case above");
    real = lxb_dom_interface_node(src->document)->owner_document;
    DCHECK(real != NULL && real != src_doc, "§13.4's temporary document has no owner — lxb_dom_document_init "
                                            "with a non-NULL owner is what inherits the arenas every node of "
                                            "this parse was allocated from");
    /* THE PARTIAL TREE IS THE TEMPORARY DOCUMENT'S ONE CHILD. chunk_begin inserts §13.4 step 8's root element
       there and the tree builder inserts everything else under it, so the walk below has exactly one entry
       point and the caller's `root` is the copy of it. */
    DCHECK(src_doc->node.first_child == src_root && src_doc->node.last_child == src_root,
           "§13.4's temporary document does not hold the parse's root element as its only child — the copy "
           "walks that document's children, so a second one is a subtree it would silently place under the "
           "wrong parent");
    DCHECK(src_doc->element == lxb_dom_interface_element(src_root),
           "§13.4's temporary document does not name the parse's root element as its document element — "
           "chunk_begin attaches it, and lexbor's tree builder reads it back");

    /* THE COPY'S OWN TEMPORARY DOCUMENT, made exactly as chunk_begin makes the original's: against the REAL
       document, so it inherits the same arenas and the same name hashes. It has to be its own, because
       chunk_destroy DESTROYS a non-original tree->document at the end of a parse — one shared between two arms
       would be freed under whichever arm finished second, and §13.4's root element hangs off it. */
    out->document = lxb_html_document_interface_create(lxb_html_interface_document(real));
    CHECK(out->document != NULL, "the fragment-parse copy could not create its temporary document");
    copy_doc = lxb_dom_interface_document(out->document);
    /* INHERITED, NOT COPIED — and asserted, because the day lxb_dom_document_init stops inheriting one of these
       the copy would parse under a different scripting mode or a different quirks mode than the arm it was
       forked from, and the two arms would diverge on markup neither of them chose. */
    DCHECK(copy_doc->scripting == src_doc->scripting && copy_doc->compat_mode == src_doc->compat_mode,
           "the fragment-parse copy's temporary document did not inherit the source's scripting and quirks "
           "modes — lxb_dom_document_init takes both from the owner, and both arms were given the same owner");

    map_begin(&map);
    copy_subtree(out->document, &src_doc->node, &copy_doc->node, &map);
    out->root = copy_doc->node.first_child;
    DCHECK(out->root == map_get(&map, src_root),
           "the copy of §13.4's root element is not the copy document's first child — the walk copies the "
           "source document's children in order into a tree that starts empty");
    lxb_dom_document_attach_element(copy_doc, lxb_dom_interface_element(out->root));

    copy = lxb_html_tree_create();
    CHECK(copy != NULL, "the fragment-parse copy could not create its tree builder");
    /* AND THIS IS WHERE THE TWO HALVES BIND. lxb_html_tree_init takes the tokenizer's REFERENCE (tkz_ref) and
       points its token-done callback at this tree — which is why `tkz` must be the copy's own tokenizer and
       why element.c's frag_unforkable says the two halves cannot be cloned separately. It also allocates the
       four arrays and the parse-error list, which is why every field below is a copy INTO them rather than a
       pointer taken FROM the source. */
    CHECK(lxb_html_tree_init(copy, tkz) == LXB_STATUS_OK,
          "the fragment-parse copy could not initialise its tree builder");
    /* `ref_count` IS NOT COPIED, deliberately. lxb_html_tree_init leaves it at 1 and the copy has exactly one
       owner by construction — the parser the caller assembles around it. Copying the source's would leave the
       sibling's unref never reaching zero, so the tree, its five arrays and its temporary document would
       outlive the parse with nothing naming them. */
    DCHECK(copy->ref_count == 1, "a freshly initialised lxb_html_tree does not hold one reference — the copy's "
                                 "single owner is the parser the caller builds around it");

    lxb_html_tree_attach_document(copy, out->document);

    /* §13.4 step 3's CONTEXT ELEMENT. It is what lxb_html_tree_adjusted_current_node answers with once the
       stack of open elements is down to the root, so a copy that shared the original's would read a node the
       other arm's chunk_destroy frees. */
    copy->fragment = copy_detached(out->document, src->fragment, &map);
    /* §13.4 step 5's SYNTHETIC FORM — created only when the context element is a `<form>`, and the caller's,
       because a `</form>` end tag clears `tree->form` and the parser's pointer is then the only one left. */
    out->form = src_form != NULL ? copy_detached(out->document, src_form, &map) : NULL;

    /* THE FORM ELEMENT POINTER (§13.2.4.3). Either a form the parse built — which the subtree walk copied — or
       step 5's synthetic one, which copy_detached just entered in the map. One lookup answers both. */
    if (src->form != NULL) {
        lxb_dom_node_t *f = map_get(&map, lxb_dom_interface_node(src->form));
        DCHECK(f != NULL,
               "the form element pointer names a node this copy never reached — a form the parse inserted is "
               "under the root, and §13.4 step 5's is the parser's, so a third kind is a node the walk has no "
               "way to have copied");
        copy->form = lxb_html_interface_form(f);
    }

    /* THE STACK OF OPEN ELEMENTS (§13.2.4.2), remapped entry by entry. A source entry with no copy is the one
       failure this whole file exists to make impossible — it would leave the sibling's stack naming the
       original arm's nodes, so both arms would insert into one tree. */
    n = lexbor_array_length(src->open_elements);
    for (i = 0; i < n; i++) {
        lxb_dom_node_t *e = map_get(&map, (lxb_dom_node_t *)lexbor_array_get(src->open_elements, i));
        DCHECK(e != NULL,
               "an element on the stack of open elements was not reached by the copy walk — every element the "
               "tree builder pushes is one it inserted into the parse's own tree, so this is a node the walk "
               "cannot see and the sibling's stack would name the original arm's");
        CHECK(lexbor_array_push(copy->open_elements, e) == LXB_STATUS_OK,
              "the fragment-parse copy could not grow its stack of open elements");
    }

    /* THE LIST OF ACTIVE FORMATTING ELEMENTS (§13.2.4.3), and its MARKERS. A marker is one static sentinel
       lexbor hands out from lxb_html_tree_active_formatting_marker(); it is not a node of anybody's tree and
       both arms compare against the same address, so it passes through unremapped. Remapping it would put a
       COPY of the sentinel in the list, and every "up to the last marker" walk would run off the end. */
    n = lexbor_array_length(src->active_formatting);
    for (i = 0; i < n; i++) {
        lxb_dom_node_t *e = (lxb_dom_node_t *)lexbor_array_get(src->active_formatting, i);
        if (e != lxb_dom_interface_node(lxb_html_tree_active_formatting_marker())) {
            e = map_get(&map, e);
            DCHECK(e != NULL,
                   "an element on the list of active formatting elements was not reached by the copy walk — "
                   "the list holds elements the parse inserted and the one marker sentinel, and neither of "
                   "those is a node outside the parse's tree");
        }
        CHECK(lexbor_array_push(copy->active_formatting, e) == LXB_STATUS_OK,
              "the fragment-parse copy could not grow its list of active formatting elements");
    }

    /* THE STACK OF TEMPLATE INSERTION MODES (§13.2.4.1). Each entry is one insertion-mode function — code, the
       same 26 addresses in both arms — so it is copied and not remapped. */
    n = lexbor_array_obj_length(src->template_insertion_modes);
    for (i = 0; i < n; i++) {
        lxb_html_tree_template_insertion_t *e = lexbor_array_obj_push(copy->template_insertion_modes);
        CHECK(e != NULL, "the fragment-parse copy could not grow its stack of template insertion modes");
        *e = *(const lxb_html_tree_template_insertion_t *)
              lexbor_array_obj_get(src->template_insertion_modes, i);
    }

    /* THE PENDING TABLE CHARACTER TOKENS (§13.2.6.4.10). These are lexbor_str_t values whose BYTES live in the
       real document's `text` arena, and in_table_text's own erase destroys them — so the two arms cannot share
       one allocation, or the first arm to leave "in table text" frees bytes the second is still holding. Each
       is copied into the same arena, which both arms already allocate every string of the parse from. */
    n = lexbor_array_obj_length(src->pending_table.text_list);
    for (i = 0; i < n; i++) {
        const lexbor_str_t *s = lexbor_array_obj_get(src->pending_table.text_list, i);
        lexbor_str_t *d = lexbor_array_obj_push(copy->pending_table.text_list);
        CHECK(d != NULL, "the fragment-parse copy could not grow its pending table character tokens");
        CHECK(lexbor_str_copy(d, s, real->text) != NULL,
              "the fragment-parse copy could not copy a pending table character token");
    }
    copy->pending_table.have_non_ws = src->pending_table.have_non_ws;

    /* THE PARSE ERRORS. `id` is tree-construction state — which rule was violated, in order — and `begin`/`end`
       are the token EXTENT, which names §13.2.5's incoming buffer and is therefore moved by the caller's own
       rebase rather than by anything this half could know. */
    n = lexbor_array_obj_length(src->parse_errors);
    for (i = 0; i < n; i++) {
        const lxb_html_tree_error_t *s = lexbor_array_obj_get(src->parse_errors, i);
        lxb_html_tree_error_t *d = lexbor_array_obj_push(copy->parse_errors);
        CHECK(d != NULL, "the fragment-parse copy could not grow its parse-error list");
        d->id = s->id;
        d->begin = rebase(s->begin, rebase_ctx);
        d->end = rebase(s->end, rebase_ctx);
    }

    /* THE INSERTION MODE and the one it returns to (§13.2.4.1) — insertion-mode functions, code, the same
       address in both arms, so they are copied and never remapped. `before_append_attr` is the attribute
       adjustment a foreign start tag installs and clears within its own token; it is a field all the same, and
       a field a copy skips is a field nothing reports as missing. */
    copy->mode = src->mode;
    copy->original_mode = src->original_mode;
    copy->before_append_attr = src->before_append_attr;
    /* §13.2.6.1's foster parenting flag and §13.2.4's frameset-ok and scripting flags, and lexbor's own running
       status. THAT IS EVERY FIELD OF lxb_html_tree, and the count is the point: seventeen, of which twelve are
       what element.c's frag_unforkable names (the two modes, the three stacks, the pending table, the parse
       errors, the form and context pointers, and the three flags), `document` and `tkz_ref` are the two halves
       this operation binds, `before_append_attr` and `status` are here, and `ref_count` is the one field
       deliberately NOT copied — see the DCHECK above. A field added to lexbor's struct and not to this list is
       a field the sibling arm silently starts a parse without. */
    copy->foster_parenting = src->foster_parenting;
    copy->frameset_ok = src->frameset_ok;
    copy->scripting = src->scripting;
    copy->status = src->status;

    map_end(&map);
    out->tree = copy;
}
