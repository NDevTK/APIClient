/* DOM §4.11 Interface Text's child text content and descendant text content. See core/dom/text_content.h.
 *
 * TWO PASSES, NOT A GROWING BUFFER, and the reason is the same one that makes this a component: the answer is
 * the concatenation of data the tree already holds, so its length is a fact the tree can be asked for before a
 * byte is copied. One measure and one copy allocate exactly once and cannot disagree about what counts,
 * because both passes are the SAME walk over the SAME predicate — written once here as a callback rather than
 * twice as two loops, which is where a measure-and-copy pair usually goes wrong.
 *
 * THE WALK IS ITERATIVE AND CARRIES NO C STACK. CLAUDE.md §The-C-stack-is-a-NON-limit makes recursion depth a
 * property of the heap and not of this frame, and a document's nesting depth is response-controlled, so a
 * recursive descent here would be a bound this file gets to choose. The descendant walk is DOM §1.1's own
 * pre-order over child links, expressed with a cursor and a limit. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/dom/text_content.h"

bool dom_node_is_text(const lxb_dom_node_t *node)
{
    DCHECK(node != NULL,
           "DOM §4.12 Interface CDATASection's interface question was asked of no node — every caller reaches "
           "this from a link it just followed, so a NULL here is a walk that read past the end of one");
    /* §4.12: `interface CDATASection : Text { };`. The two nodeTypes are ONE interface for every algorithm
       DOM writes in terms of "Text node", which is what §4.4 Interface Node means by "switching on the
       interface node implements". */
    return node->type == LXB_DOM_NODE_TYPE_TEXT || node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION;
}

/* THE DATA OF ONE TEXT NODE, which is DOM §4.10 Interface CharacterData's `data` — reached through the
   CharacterData layout a Text and a CDATASection share, because sharing it is what `CDATASection : Text`
   MEANS. Asserted rather than assumed: a node this returns bytes for has already answered the interface
   question above, so a kind whose layout is not CharacterData's cannot arrive. */
static const lxb_char_t *txt_data(const lxb_dom_node_t *n, size_t *len)
{
    const lxb_dom_character_data_t *cd;

    DCHECK(dom_node_is_text(n),
           "DOM §4.10 Interface CharacterData's `data` was read off a node that is not a Text node — the "
           "concatenations in this file range over Text nodes only, so a node reaching here that failed "
           "dom_node_is_text is a walk that appended before it tested");
    cd = (const lxb_dom_character_data_t *)n;
    *len = cd->data.length;
    return cd->data.data;
}

/* THE RESULT BUFFER, WRITTEN ONCE. `total` is what the measuring pass counted; a copying pass that wrote a
   different number of bytes counted one thing and copied another, which the assert at the bottom of each
   entry is what catches. */
static char *txt_alloc(size_t total)
{
    char *out = malloc(total + 1);

    CHECK(out != NULL,
          "OOM materializing DOM §4.11 Interface Text's text content — the callers of this are a `<script>`'s "
          "source text, a `<style>`'s rules and `textContent`, so an allocation that fails here is a document "
          "whose own program silently does not run rather than a document with no program");
    out[total] = '\0';
    return out;
}

char *dom_child_text_content(const lxb_dom_node_t *node, size_t *len)
{
    const lxb_dom_node_t *c;
    size_t total = 0, at = 0, n;
    char *out;

    DCHECK(node != NULL && len != NULL,
           "DOM §4.11 Interface Text's child text content was asked of no node, or with nowhere to put the "
           "length — the length is not optional, because §4.12.1's inline source text is the one string in "
           "this engine that can hold a U+0000 and a caller that measured it with strlen has truncated a "
           "page's own program");
    /* "…the concatenation of the data of all the Text node children of node, in tree order." CHILDREN, so one
       level of links and no descent. */
    for (c = node->first_child; c != NULL; c = c->next)
        if (dom_node_is_text(c)) { (void)txt_data(c, &n); total += n; }
    out = txt_alloc(total);
    for (c = node->first_child; c != NULL; c = c->next) {
        const lxb_char_t *d;

        if (!dom_node_is_text(c)) continue;
        d = txt_data(c, &n);
        DCHECK(at + n <= total,
               "DOM §4.11's child text content copied more bytes than its own measuring pass counted — the "
               "two passes walk the same links over the same predicate, so a disagreement means the tree "
               "changed between them, which nothing in this function can do");
        memcpy(out + at, d, n);
        at += n;
    }
    DCHECK(at == total, "DOM §4.11's child text content copied fewer bytes than it counted — same walk, same "
                        "predicate, so the two passes cannot see a different set of children");
    *len = total;
    return out;
}

/* DOM §1.1's PRE-ORDER over child links, bounded by the subtree `root` — the next node after `n`, or NULL when
   the walk has returned to `root`. Written as a step so both passes below take the identical path. */
static const lxb_dom_node_t *txt_next(const lxb_dom_node_t *n, const lxb_dom_node_t *root)
{
    if (n->first_child != NULL) return n->first_child;
    while (n != root) {
        if (n->next != NULL) return n->next;
        n = n->parent;
        DCHECK(n != NULL,
               "a descendant walk climbed out of the subtree it was given without meeting its root — the "
               "parent chain of a node inside a subtree reaches that subtree's root, so a NULL here is a node "
               "whose parent link does not agree with the tree it was reached through");
    }
    return NULL;
}

char *dom_descendant_text_content(const lxb_dom_node_t *node, size_t *len)
{
    const lxb_dom_node_t *c;
    size_t total = 0, at = 0, n;
    char *out;

    DCHECK(node != NULL && len != NULL,
           "DOM §4.11 Interface Text's descendant text content was asked of no node, or with nowhere to put "
           "the length — see dom_child_text_content for why the length is not optional");
    /* "…the concatenation of the data of all the Text node descendants of node, in tree order." The ROOT is
       not its own descendant, so the walk starts at its first child. */
    for (c = txt_next(node, node); c != NULL; c = txt_next(c, node))
        if (dom_node_is_text(c)) { (void)txt_data(c, &n); total += n; }
    out = txt_alloc(total);
    for (c = txt_next(node, node); c != NULL; c = txt_next(c, node)) {
        const lxb_char_t *d;

        if (!dom_node_is_text(c)) continue;
        d = txt_data(c, &n);
        DCHECK(at + n <= total,
               "DOM §4.11's descendant text content copied more bytes than its own measuring pass counted — "
               "the two passes walk the same links over the same predicate, so a disagreement means the tree "
               "changed between them, which nothing in this function can do");
        memcpy(out + at, d, n);
        at += n;
    }
    DCHECK(at == total, "DOM §4.11's descendant text content copied fewer bytes than it counted — same walk, "
                        "same predicate, so the two passes cannot see a different set of descendants");
    *len = total;
    return out;
}
