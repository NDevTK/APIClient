/* A Document's name index, Blink's per-tree-scope named-item map in a weaker form: for each document, the
 * multiset of null-namespace `id` and `name` attribute values over the elements in its document tree. It is a
 * function of the tree, maintained at lexbor's tree observer (core primitives every link and attribute write
 * passes, the parser's and the COW delta's swap included), so it is the running flow's tree's own and needs no
 * capture: a swap re-links nodes through the same primitives. A count never goes below zero (asserted), which
 * a write through an unobserved primitive would break first, and document_name_index_verify_all recounts every
 * live document's tree against its index (dev builds). The multiset is a superset of HTML §3.1.7's supported
 * property names (which also test the element type), so a hit still runs the walk that decides. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/dom/interfaces/attr_const.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "core/dom/document_name_index.h"
#include "core/timing/thread_work.h"
#include "solver/dom_cow.h"   /* whether a write is a context switch's restore */

typedef struct {
    char    *key;      /* owned; NULL for an empty slot */
    size_t   len;
    uint64_t hash;
    /* The elements in the tree carrying the value, one entry per attribute that carries it, in tree order (an
       element whose id and name both carry it holds two adjacent entries); owned. An empty list keeps the slot
       for reuse. */
    lxb_dom_element_t **els;
    size_t   count, cap;
} DniSlot;

/* A value held by some element, in the map's ordered list of held values. The key bytes are the slot's, which a
   rehash moves by value, so the pointer stays valid. */
typedef struct {
    const char *key;
    size_t      len;
} DniHeld;

typedef struct DniMap {
    DniSlot *slots;
    size_t   cap, used;   /* cap a power of two; used counts keyed slots */
    /* The values with a non-empty list, ordered by their bytes: a function of the tree, so a resting walk over the
       values resumes at the same one. */
    DniHeld *held;
    size_t   nheld, cap_held;
    lxb_dom_document_t *doc;
    struct DniMap *prev, *next;   /* every live document's, from its creation to its destroy */
} DniMap;

static bool g_installed;
static DniMap *g_maps;
/* Nodes the current observer call stepped over (its subtree's and its tree-order comparisons'), charged as
   DOM-walk work when the call returns: one charge per call, so a call's rounding does not grow with its steps. */
static uint64_t g_steps;

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
    size_t cap = m->cap ? m->cap * 2 : 64;
    DniSlot *old = m->slots;
    size_t old_cap = m->cap;

    m->slots = calloc(cap, sizeof(DniSlot));
    CHECK(m->slots != NULL, "dom-name-index-oom: the name index could not grow");
    m->cap = cap;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i].key != NULL) *dni_find(m, old[i].key, old[i].len, old[i].hash) = old[i];
    free(old);
}

/* Whether connected node `a` precedes connected node `b` of the same document tree in tree order (DOM §4.2.1
   "Document tree"'s preorder, depth-first traversal). An ancestor precedes its descendants; siblings are ordered
   by a scan out from `a` in both directions, which costs their distance. */
static bool dni_precedes(lxb_dom_node_t *a, lxb_dom_node_t *b)
{
    int da = 0, db = 0;

    if (a == b) return false;
    for (lxb_dom_node_t *x = a; x->parent; x = x->parent) da++;
    for (lxb_dom_node_t *x = b; x->parent; x = x->parent) db++;
    g_steps += (uint64_t)da + (uint64_t)db;
    for (; da > db; da--) { a = a->parent; if (a == b) return false; }
    for (; db > da; db--) { b = b->parent; if (b == a) return true; }
    while (a->parent != b->parent) { a = a->parent; b = b->parent; }
    DCHECK(a->parent != NULL, "two elements of one name index are in no common tree");
    for (lxb_dom_node_t *x = a->next, *y = a->prev; x || y; x = x ? x->next : NULL, y = y ? y->prev : NULL) {
        g_steps++;
        if (x == b) return true;
        if (y == b) return false;
    }
    DFAIL("two siblings of one parent were not found on its child list from either side");
    return false;
}

/* The first position in `s`'s list whose element does not precede `el`. */
static size_t dni_lower_bound(const DniSlot *s, lxb_dom_element_t *el)
{
    size_t lo = 0, hi = s->count;

    if (s->count && dni_precedes(lxb_dom_interface_node(s->els[s->count - 1]), lxb_dom_interface_node(el)))
        return s->count;   /* a parse appends in tree order */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (dni_precedes(lxb_dom_interface_node(s->els[mid]), lxb_dom_interface_node(el))) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Byte order of two values, the shorter first on a common prefix. */
static int dni_bytes_cmp(const char *a, size_t al, const char *b, size_t bl)
{
    int c = memcmp(a, b, al < bl ? al : bl);

    return c != 0 ? c : (al < bl ? -1 : al > bl ? 1 : 0);
}

/* The first position of `m`'s held values not ordered before `k`. */
static size_t dni_held_at(const DniMap *m, const char *k, size_t n)
{
    size_t lo = 0, hi = m->nheld;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (dni_bytes_cmp(m->held[mid].key, m->held[mid].len, k, n) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* `s`'s value entered (its list became non-empty) or left (it became empty) the held values. */
static void dni_held_update(DniMap *m, const DniSlot *s, bool entered)
{
    size_t at = dni_held_at(m, s->key, s->len);

    if (entered) {
        DCHECK(at == m->nheld || dni_bytes_cmp(m->held[at].key, m->held[at].len, s->key, s->len) != 0,
               "a value entered a name index's held values twice");
        if (m->nheld == m->cap_held) {
            m->cap_held = m->cap_held ? m->cap_held * 2 : 16;
            m->held = realloc(m->held, m->cap_held * sizeof *m->held);
            CHECK(m->held != NULL, "dom-name-index-oom: the held values could not grow");
        }
        memmove(&m->held[at + 1], &m->held[at], (m->nheld - at) * sizeof *m->held);
        m->held[at].key = s->key;
        m->held[at].len = s->len;
        m->nheld++;
        return;
    }
    DCHECK(at < m->nheld && m->held[at].key == s->key, "a value left a name index's held values it is not among");
    memmove(&m->held[at], &m->held[at + 1], (m->nheld - at - 1) * sizeof *m->held);
    m->nheld--;
}

/* Enter `el` into `m`'s list for the `n` bytes of `v` (delta +1) or take one entry of it out (delta -1). The list's
   order is a function of the tree, so a resumed walk's position in it names the same element. */
static void dni_map_adjust(DniMap *m, const lxb_char_t *v, size_t n, lxb_dom_element_t *el, int delta)
{
    size_t at;

    uint64_t h = dni_hash((const char *)v, n);
    DniSlot *s;

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
    at = dni_lower_bound(s, el);
    if (delta > 0) {
        if (s->count == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 2;
            s->els = realloc(s->els, s->cap * sizeof *s->els);
            CHECK(s->els != NULL, "dom-name-index-oom: a name's element list could not grow");
        }
        memmove(&s->els[at + 1], &s->els[at], (s->count - at) * sizeof *s->els);
        s->els[at] = el;
        if (s->count++ == 0) dni_held_update(m, s, true);
        return;
    }
    DCHECK(at < s->count && s->els[at] == el,
           "an element left a name's list it is not on: a tree or attribute write reached the tree through a "
           "primitive lexbor's tree observer does not see");
    memmove(&s->els[at], &s->els[at + 1], (s->count - at - 1) * sizeof *s->els);
    if (--s->count == 0) dni_held_update(m, s, false);
}

static void dni_adjust(lxb_dom_document_t *doc, const lxb_char_t *v, size_t n, lxb_dom_element_t *el, int delta)
{
    DCHECK(doc->name_index != NULL, "a document's tree changed and its name index was never made: the document "
                                    "was initialised before the index was lexbor's tree observer");
    dni_map_adjust(doc->name_index, v, n, el, delta);
}

/* The document whose document tree `n` is in, or NULL: the root of its parent chain is a document that holds
   the chain's top as a child. A fragment parse links its root to the document without being its child. */
static lxb_dom_document_t *dni_tree_document(lxb_dom_node_t *n)
{
    for (; n->parent != NULL; n = n->parent) {
        g_steps++;
        if (n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            lxb_dom_node_t *c = n->parent->first_child;

            while (c != NULL && c != n) { g_steps++; c = c->next; }
            return c != NULL ? lxb_dom_interface_document(n->parent) : NULL;
        }
    }
    return NULL;
}

/* Whether `n` is in `doc`'s document tree: dni_tree_document's question, uncharged, for an assert. */
static bool dni_linked_in(const lxb_dom_node_t *n, const lxb_dom_document_t *doc)
{
    for (; n->parent != NULL; n = n->parent)
        if (n->parent == lxb_dom_interface_node(doc)) {
            const lxb_dom_node_t *c = n->parent->first_child;

            while (c != NULL && c != n) c = c->next;
            return c != NULL;
        }
    return false;
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
        if (dni_value(a, &v, &n)) dni_adjust(doc, v, n, el, delta);
}

/* The current call's steps, charged as THREAD_WORK_DOM_WALK unless a context switch's delta restore made the
   write, which is no flow's act.
   Named residual. Not covered: an insertion's upkeep other than core/dom/node_insert.h's machine, and a
   removal's other than core/dom/node_remove.h's, runs inside the tree write's primitive, charged but not resting,
   so a fragment-inserted or innerHTML-cleared subtree's walk holds the thread. Next: every insert and removal
   routed through those machines, which the observer's subtree walks then leave. Absence shows as a flow's slice
   overrun inside a tree write with DOM-walk work charged across it. */
static void dni_charge(void)
{
    if (g_steps && !dom_cow_restoring()) thread_work_charge(THREAD_WORK_DOM_WALK, g_steps);
    g_steps = 0;
}

/* Every element of `root`'s inclusive subtree, in tree order. */
static void dni_subtree(lxb_dom_node_t *root, int delta)
{
    lxb_dom_document_t *doc = dni_tree_document(root);

    if (doc == NULL) return;
    for (lxb_dom_node_t *n = root; n != NULL;) {
        g_steps++;
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) dni_element(doc, lxb_dom_interface_element(n), delta);
        if (n->first_child != NULL) { n = n->first_child; continue; }
        while (n != root && n->next == NULL) n = n->parent;
        n = n == root ? NULL : n->next;
    }
}

/* A link solver/dom_cow.c makes with the subtree's upkeep carried per element (dom_cow_name_index_apart) walks
   nothing here: the elements left, or rejoin, the index through document_name_index_element. */
static void dni_inserted(lxb_dom_node_t *node)
{
    if (!dom_cow_name_index_apart()) dni_subtree(node, +1);
    dni_charge();
}
static void dni_removing(lxb_dom_node_t *node)
{
    if (node->parent != NULL && !dom_cow_name_index_apart()) dni_subtree(node, -1);
    dni_charge();
}

lxb_dom_document_t *document_name_index_tree_document(lxb_dom_node_t *n)
{
    lxb_dom_document_t *doc;

    DCHECK(g_installed, "a name index's document was asked before the index was lexbor's tree observer");
    DCHECK(n != NULL, "a name index's document was asked of no node");
    doc = dni_tree_document(n);
    dni_charge();
    return doc;
}

bool document_name_index_element(lxb_dom_document_t *doc, lxb_dom_element_t *el, int delta)
{
    bool held = false;
    const lxb_char_t *v;
    size_t n;

    DCHECK(doc != NULL && el != NULL && (delta == 1 || delta == -1),
           "an element's name-index values were adjusted with no document, no element or a delta that is not one");
    DCHECKF(dni_linked_in(lxb_dom_interface_node(el), doc),
            "an element's name-index values were adjusted for a document whose tree it is not in (delta %d): its "
            "values are a function of that tree only while it is linked there", delta);
    for (lxb_dom_attr_t *a = el->first_attr; a != NULL; a = a->next)
        if (dni_value(a, &v, &n)) { dni_adjust(doc, v, n, el, delta); held = true; }
    dni_charge();
    return held;
}

static void dni_attr(lxb_dom_element_t *el, lxb_dom_attr_t *a, int delta)
{
    const lxb_char_t *v;
    size_t n;
    lxb_dom_document_t *doc;

    if (!dni_value(a, &v, &n)) return;
    doc = dni_tree_document(lxb_dom_interface_node(el));
    if (doc != NULL) dni_adjust(doc, v, n, el, delta);
}

static void dni_attr_added(lxb_dom_element_t *el, lxb_dom_attr_t *a) { dni_attr(el, a, +1); dni_charge(); }
static void dni_attr_removing(lxb_dom_element_t *el, lxb_dom_attr_t *a) { dni_attr(el, a, -1); dni_charge(); }

static void dni_map_free(DniMap *m)
{
    for (size_t i = 0; i < m->cap; i++) { free(m->slots[i].key); free(m->slots[i].els); }
    free(m->slots);
    free(m->held);
    free(m);
}

static void dni_document_created(lxb_dom_document_t *doc)
{
    DniMap *m = calloc(1, sizeof *m);

    CHECK(m != NULL, "dom-name-index-oom: the name index could not be allocated");
    DCHECK(doc->name_index == NULL, "a document was initialised while it still held a name index");
    m->doc = doc;
    m->next = g_maps;
    if (g_maps) g_maps->prev = m;
    g_maps = m;
    doc->name_index = m;
}

/* Idempotent: a document's destroy can pass more than one observed exit. */
static void dni_document_destroying(lxb_dom_document_t *doc)
{
    DniMap *m = doc->name_index;

    if (m == NULL) return;
    if (m->prev) m->prev->next = m->next; else g_maps = m->next;
    if (m->next) m->next->prev = m->prev;
    dni_map_free(m);
    doc->name_index = NULL;
}

static const lxb_dom_tree_observer_t DNI_OBSERVER = {
    dni_inserted, dni_removing, dni_attr_added, dni_attr_removing, dni_document_created, dni_document_destroying,
};

void document_name_index_install(void)
{
    DCHECK(lxb_dom_tree_observer() == NULL || lxb_dom_tree_observer() == &DNI_OBSERVER,
           "lexbor's tree observer is held by another component: the two have to be composed into one table");
    lxb_dom_tree_observer_set(&DNI_OBSERVER);
    g_installed = true;
}

size_t document_name_index_list(lxb_dom_node_t *doc, const char *name, size_t n, lxb_dom_element_t *const **out)
{
    DniMap *m;
    DniSlot *s;

    DCHECK(g_installed, "a document's name index was asked before it was installed as lexbor's tree observer");
    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT && out != NULL,
           "a name index was asked of no document, or with nowhere to put its list");
    m = lxb_dom_interface_document(doc)->name_index;
    DCHECK(m != NULL, "a document's name index was asked and the document has none: it was initialised before the "
                      "index was lexbor's tree observer");
    *out = NULL;
    if (n == 0 || m->cap == 0) return 0;   /* a map holds slots only once a value entered it */
    s = dni_find(m, name, n, dni_hash(name, n));
    if (s->key == NULL) return 0;
    *out = s->els;
    return s->count;
}

size_t document_name_index_values(lxb_dom_node_t *doc)
{
    DniMap *m;

    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT, "a name index's values were asked of no document");
    m = lxb_dom_interface_document(doc)->name_index;
    DCHECK(m != NULL, "a document's name index was asked and the document has none");
    return m->nheld;
}

const char *document_name_index_value_at(lxb_dom_node_t *doc, size_t i, size_t *len)
{
    DniMap *m = lxb_dom_interface_document(doc)->name_index;

    DCHECKF(m != NULL && i < m->nheld, "a name index's value %zu was asked of %zu", i, m ? m->nheld : (size_t)0);
    *len = m->held[i].len;
    return m->held[i].key;
}

bool document_name_index_holds(lxb_dom_node_t *doc, const char *name, size_t n)
{
    lxb_dom_element_t *const *els;

    return document_name_index_list(doc, name, n, &els) > 0;
}

#if APICLIENT_DEV
/* The first value whose list in `a` is not the same list, in the same order, in `b`; its length in `b` at
   `*other`. */
static const DniSlot *dni_first_unequal(const DniMap *a, const DniMap *b, size_t *other)
{
    for (size_t i = 0; i < a->cap; i++) {
        const DniSlot *s = &a->slots[i], *t;

        if (s->key == NULL || s->count == 0) continue;
        t = b->cap ? dni_find((DniMap *)b, s->key, s->len, s->hash) : NULL;
        *other = (t != NULL && t->key != NULL) ? t->count : 0;
        if (*other != s->count || memcmp(s->els, t->els, s->count * sizeof *s->els) != 0) return s;
    }
    return NULL;
}
#endif

void document_name_index_verify_all_at(const char *file, int line)
{
#if APICLIENT_DEV
    DCHECK(g_installed, "the name indexes were verified before the index was lexbor's tree observer");
    for (DniMap *m = g_maps; m != NULL; m = m->next) {
        lxb_dom_node_t *root = lxb_dom_interface_node(m->doc);
        DniMap re = { 0 };
        const DniSlot *s;
        size_t other = 0;
        bool in_index;

        /* The recount: every element below the document node is in its document tree. */
        for (lxb_dom_node_t *n = root->first_child; n != NULL;) {
            if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                const lxb_char_t *v;
                size_t vl;

                for (lxb_dom_attr_t *a = lxb_dom_interface_element(n)->first_attr; a != NULL; a = a->next)
                    if (dni_value(a, &v, &vl)) dni_map_adjust(&re, v, vl, lxb_dom_interface_element(n), +1);
            }
            if (n->first_child != NULL) { n = n->first_child; continue; }
            while (n != root && n->next == NULL) n = n->parent;
            n = n == root ? NULL : n->next;
        }
        in_index = (s = dni_first_unequal(m, &re, &other)) != NULL;
        if (!in_index) s = dni_first_unequal(&re, m, &other);
        DCHECKF(s != NULL || re.nheld == m->nheld, "%s:%d: a document's name index holds %zu values and its tree "
                "%zu", file, line, m->nheld, re.nheld);
        for (size_t i = 0; s == NULL && i < m->nheld; i++)
            DCHECKF(dni_bytes_cmp(m->held[i].key, m->held[i].len, re.held[i].key, re.held[i].len) == 0,
                    "%s:%d: a document's name index orders its held values unlike its tree's at %zu", file, line, i);
        DCHECKF(s == NULL,
                "%s:%d: a document's name index disagrees with its tree at `%.*s`: the %s lists %zu elements and "
                "the %s %zu, or both list them in another order, so a tree or attribute write reached the tree "
                "through a primitive lexbor's tree observer does not see", file, line, s ? (int)s->len : 0,
                s ? s->key : "", in_index ? "index" : "tree", s ? s->count : (size_t)0, in_index ? "tree" : "index",
                other);
        for (size_t i = 0; i < re.cap; i++) { free(re.slots[i].key); free(re.slots[i].els); }
        free(re.slots);
        free(re.held);
    }
    g_steps = 0;   /* the recount's comparisons are a dev check, charged to no flow */
#else
    (void)file; (void)line;
#endif
}
