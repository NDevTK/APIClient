/* The named getters' resting tree walk — see named_walk.h. */
#include "core/dom/named_walk.h"

#include "check.h"
#include "core/timing/thread_work.h"
#include "solver/concolic.h"

lxb_dom_node_t *named_walk_successor(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    DCHECK(n != NULL && root != NULL, "a named walk stepped from no node or within no root");
    do {
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) return NULL;
        n = n->next;
    } while (n->type != LXB_DOM_NODE_TYPE_ELEMENT);
    return n;
}

bool named_walk_next(JSContext *ctx, NamedWalk *w, lxb_dom_node_t *root, NamedWalkIs is, const char *name,
                     size_t n, const void *ud)
{
    DCHECK(w != NULL && root != NULL && is != NULL, "a named walk was stepped with no state, root or rule");
    DCHECK(w->count < 2 && (w->root == NULL || w->root == root),
           "a named walk was stepped after it ended, or over a root other than the one it began with");
    if (!w->root) { w->root = root; w->cursor = root; }
    w->cursor = named_walk_successor(w->cursor, root);
    if (!w->cursor) return false;
    thread_work_charge(THREAD_WORK_DOM_WALK, 1);
    if (is(ctx, lxb_dom_interface_element(w->cursor), name, n, ud)) {
        if (!w->count) w->first = w->cursor;
        w->count++;
    }
    return w->count < 2;
}

bool named_walk_names_in_domain_at(JSContext *ctx, lxb_dom_element_t *el, const lxb_char_t *const v[],
                                   const size_t len[], int k, NamedWalkIs is, const void *ud, JSValueConst key,
                                   const char *file, int line)
{
    DCHECK(el != NULL && is != NULL && k >= 0 && k <= 2, "a named-name domain test was handed no element or rule");
    for (int i = 0; i < k; i++) {
        DCHECKF(is(ctx, el, (const char *)v[i], len[i], ud),
                "%s:%d: a name the rule's names list gives an element is not one its named lookup answers for it, "
                "so the domain answer and the [[Get]] it stands for are two facts", file, line);
        if (concolic_key_domain_holds(ctx, key, (const char *)v[i], len[i])) return true;
    }
    (void)file; (void)line;
    return false;
}
