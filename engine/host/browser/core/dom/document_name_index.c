/* A Document's name index, Blink's per-tree-scope named-item map in a weaker form: for each document, the
 * multiset of null-namespace `id` and `name` attribute values over the elements in its document tree. It is a
 * function of the tree, maintained at lexbor's tree observer (core primitives every link and attribute write
 * passes, the parser's and the COW delta's swap included), so it is the running flow's tree's own and needs no
 * capture: a swap re-links nodes through the same primitives. A count never goes below zero (asserted), which
 * a write through an unobserved primitive would break first. The multiset is a superset of HTML §3.1.7's
 * supported property names (which also test the element type), so a hit still runs the walk that decides. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/dom/interfaces/attr_const.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "core/dom/document_name_index.h"

typedef struct {
    char    *key;      /* owned; NULL for an empty slot */
    size_t   len;
    uint64_t hash;
    uint32_t count;    /* elements in the tree carrying the value; 0 keeps the slot for reuse */
} DniSlot;

typedef struct {
    DniSlot *slots;
    size_t   cap, used;   /* cap a power of two; used counts keyed slots */
} DniMap;

static bool g_installed;

static uint64_t dni_hash(const char *k, size_t n)
{
    uint64_t h = 1469598103934665603ull;

    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)k[i]) * 1099511628211ull;
    return h;
}

/* The slot holding `k`, or the empty slot it would take. */
static DniSlot *dni_find(DniMap *m, const char *k, size_t n, uint64_t h)
{
    size_t i = (size_t)h & (m->cap - 1);

    for (;; i = (i + 1) & (m->cap - 1)) {
        DniSlot *s = &m->slots[i];

        if (s->key == NULL || (s->hash == h && s->len == n && memcmp(s->key, k, n) == 0)) return s;
    }
}

static void dni_grow(DniMap *m)
{
    DniMap g = { calloc(m->cap ? m->cap * 2 : 64, sizeof(DniSlot)), m->cap ? m->cap * 2 : 64, m->used };

    CHECK(g.slots != NULL, "dom-name-index-oom: the name index could not grow");
    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].key != NULL) *dni_find(&g, m->slots[i].key, m->slots[i].len, m->slots[i].hash) = m->slots[i];
    free(m->slots);
    *m = g;
}

static void dni_adjust(lxb_dom_document_t *doc, const lxb_char_t *v, size_t n, int delta)
{
    DniMap *m = doc->name_index;
    uint64_t h = dni_hash((const char *)v, n);
    DniSlot *s;

    if (m == NULL) {
        DCHECK(delta > 0, "a value left a document's name index that never held one: a tree or attribute write "
                          "reached the tree through a primitive lexbor's tree observer does not see");
        m = doc->name_index = calloc(1, sizeof *m);
        CHECK(m != NULL, "dom-name-index-oom: the name index could not be allocated");
    }
    if ((m->used + 1) * 4 > m->cap * 3) dni_grow(m);
    s = dni_find(m, (const char *)v, n, h);
    if (s->key == NULL) {
        DCHECK(delta > 0, "a value left a document's name index that does not hold it: a tree or attribute write "
                          "reached the tree through a primitive lexbor's tree observer does not see");
        s->key = malloc(n ? n : 1);
        CHECK(s->key != NULL, "dom-name-index-oom: a name could not be stored");
        memcpy(s->key, v, n);
        s->len = n;
        s->hash = h;
        m->used++;
    }
    DCHECK(delta > 0 || s->count > 0, "a value left a document's name index more times than it entered: a tree or "
                                      "attribute write reached the tree through a primitive the observer does "
                                      "not see");
    s->count += delta;
}

/* The document whose document tree `n` is in, or NULL: the root of its parent chain is a document that holds
   the chain's top as a child. A fragment parse links its root to the document without being its child. */
static lxb_dom_document_t *dni_tree_document(lxb_dom_node_t *n)
{
    for (; n->parent != NULL; n = n->parent) {
        if (n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            lxb_dom_node_t *c = n->parent->first_child;

            while (c != NULL && c != n) c = c->next;
            return c != NULL ? lxb_dom_interface_document(n->parent) : NULL;
        }
    }
    return NULL;
}

/* `a`'s value, when it is a non-empty null-namespace `id` or `name`. */
static bool dni_value(lxb_dom_attr_t *a, const lxb_char_t **v, size_t *n)
{
    size_t ll = 0;
    const lxb_char_t *l;

    if (a->node.ns != LXB_NS__UNDEF || a->value == NULL || a->value->data == NULL || a->value->length == 0)
        return false;
    if (a->node.local_name != LXB_DOM_ATTR_ID) {
        l = lxb_dom_attr_local_name(a, &ll);
        if (l == NULL || ll != 4 || memcmp(l, "name", 4) != 0) return false;
    }
    *v = a->value->data;
    *n = a->value->length;
    return true;
}

static void dni_element(lxb_dom_document_t *doc, lxb_dom_element_t *el, int delta)
{
    const lxb_char_t *v;
    size_t n;

    for (lxb_dom_attr_t *a = el->first_attr; a != NULL; a = a->next)
        if (dni_value(a, &v, &n)) dni_adjust(doc, v, n, delta);
}

/* Every element of `root`'s inclusive subtree, in tree order. */
static void dni_subtree(lxb_dom_node_t *root, int delta)
{
    lxb_dom_document_t *doc = dni_tree_document(root);

    if (doc == NULL) return;
    for (lxb_dom_node_t *n = root; n != NULL;) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) dni_element(doc, lxb_dom_interface_element(n), delta);
        if (n->first_child != NULL) { n = n->first_child; continue; }
        while (n != root && n->next == NULL) n = n->parent;
        n = n == root ? NULL : n->next;
    }
}

static void dni_inserted(lxb_dom_node_t *node) { dni_subtree(node, +1); }
static void dni_removing(lxb_dom_node_t *node) { if (node->parent != NULL) dni_subtree(node, -1); }

static void dni_attr(lxb_dom_element_t *el, lxb_dom_attr_t *a, int delta)
{
    const lxb_char_t *v;
    size_t n;
    lxb_dom_document_t *doc;

    if (!dni_value(a, &v, &n)) return;
    doc = dni_tree_document(lxb_dom_interface_node(el));
    if (doc != NULL) dni_adjust(doc, v, n, delta);
}

static void dni_attr_added(lxb_dom_element_t *el, lxb_dom_attr_t *a) { dni_attr(el, a, +1); }
static void dni_attr_removing(lxb_dom_element_t *el, lxb_dom_attr_t *a) { dni_attr(el, a, -1); }

static void dni_document_destroying(lxb_dom_document_t *doc)
{
    DniMap *m = doc->name_index;

    if (m == NULL) return;
    for (size_t i = 0; i < m->cap; i++) free(m->slots[i].key);
    free(m->slots);
    free(m);
    doc->name_index = NULL;
}

static const lxb_dom_tree_observer_t DNI_OBSERVER = {
    dni_inserted, dni_removing, dni_attr_added, dni_attr_removing, dni_document_destroying,
};

void document_name_index_install(void)
{
    DCHECK(lxb_dom_tree_observer() == NULL || lxb_dom_tree_observer() == &DNI_OBSERVER,
           "lexbor's tree observer is held by another component: the two have to be composed into one table");
    lxb_dom_tree_observer_set(&DNI_OBSERVER);
    g_installed = true;
}

bool document_name_index_holds(lxb_dom_node_t *doc, const char *name, size_t n)
{
    DniMap *m;
    DniSlot *s;

    DCHECK(g_installed, "a document's name index was asked before it was installed as lexbor's tree observer");
    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT, "a name index was asked of no document");
    m = lxb_dom_interface_document(doc)->name_index;
    if (m == NULL || n == 0) return false;
    s = dni_find(m, name, n, dni_hash(name, n));
    return s->key != NULL && s->count > 0;
}
