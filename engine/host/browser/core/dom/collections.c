/* NodeList and HTMLCollection — DOM §4.2.10 Old-style collections: NodeList and HTMLCollection, whose two
 * subsections are §4.2.10.1 Interface NodeList and §4.2.10.2 Interface HTMLCollection.
 *
 * THE NUMBER §4.2.11 STOOD HERE AND IN TWO COMMENTS BELOW, AND DOM HAS NO SUCH SECTION —
 * DOM §4.2 "Node tree" ends at §4.2.10 and DOM §4.3 "Mutation observers" follows it.
 * It read as authoritative for as long as nobody opened it, which is the whole
 * failure mode: a named-but-unnumbered claim can be checked, and a plausible wrong number sends the reader to a
 * section that does not exist and cannot say so.
 *
 * `childNodes`, `children` and `querySelectorAll` all answered with a plain JS ARRAY, and two of the three were
 * wrong in the way that matters: they are LIVE in the spec. A page that reads `el.children`, appends a row and
 * re-reads `.length` gets the new count in a browser and got the old one here — and a page that keeps the
 * collection across a render (which is exactly what a list component does) was iterating a corpse. The third,
 * querySelectorAll, is genuinely static, and answering all three the same way meant the difference could not
 * even be expressed.
 *
 * SO THE LIVENESS IS THE BACKING, not a refresh. A live collection holds its OWNER and counts the tree at the
 * moment it is asked; a static one holds the nodes it was built with. Both are the same object shape and both
 * reach script through the one indexed-property mechanism — what differs is four lines of backing, which is
 * the only thing that should differ.
 *
 * [SameObject] IS AN IDENTITY, so the live ones are cached on the owner's wrapper: `n.childNodes ===
 * n.childNodes` is what the IDL states, and a page that stashes the collection must be holding the same one.
 * querySelectorAll is NOT SameObject — the spec returns a new list per call — which is why it is the one that
 * takes its nodes rather than its owner.
 *
 * HTMLCollection ALSO HAS A NAMED GETTER: `form.children.foo` finds the element whose id (or, for the elements
 * that have one, name) is "foo". That is not sugar in a bundle — it is how a great deal of older code reaches
 * its own markup, and an absent named getter reads as `undefined` and takes the branch behind it with it. */
#include <stdio.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"
#include "core/realm.h"
#include "core/idl_indexed.h"
#include "core/dom/node.h"
#include "core/dom/collections.h"
#include "core/dom/document_named.h"   /* §3.1.7's named element, the document-named kind's filter */
#include "core/dom/attr_list.h"   /* dom_attr_generation, which an attribute-filtered cache keys on */
#include "core/dom/element.h"   /* element_id_bytes — DOM §4.9's ID, which both named lookups below are over */
#include "core/timing/thread_work.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"

/* PER REALM — Web IDL §3.7 "Interfaces", and here it decides ANSWERS: a C member runs in the realm
   that DEFINED it, so one shared prototype answers every document out of whichever realm built it
   first. Held in quickjs's per-context class-proto slots. */
static JSClassID g_nodelist_class, g_htmlcoll_class;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_item_id = -1, g_named_item_id = -1;

static JSValue coll_new_hc(JSContext *ctx, int kind, JSValueConst owner, const char *name, const char *ns);
static JSValue g_key = JS_UNDEFINED;        /* the collection's own slots, under a private Symbol */
static JSValue g_cache_key = JS_UNDEFINED;  /* the [SameObject] cache on an owner's wrapper */
static int     g_ready;

/* WHAT A COLLECTION IS OVER. The live kinds hold the owner and walk it per read; the static one holds an array
   and never looks at the tree again. */
/* The live kinds differ in TWO things and nothing else: which nodes they traverse, and which of those they
   take. The two child kinds walk the owner's child list; the two by-name kinds walk its whole subtree, which is
   what makes getElementsByTagName's result track a page that inserts a matching element anywhere under it. */
enum { COLL_CHILD_NODES = 0, COLL_CHILDREN, COLL_STATIC, COLL_BY_TAG, COLL_BY_TAG_NS, COLL_BY_CLASS,
       COLL_LINKS, COLL_NAMED, COLL_BY_ELEM_NAME, COLL_DOC_NAMED };

static bool coll_is_descendant(int kind)
{
    return kind == COLL_BY_TAG || kind == COLL_BY_TAG_NS || kind == COLL_BY_CLASS ||
           kind == COLL_LINKS || kind == COLL_NAMED || kind == COLL_BY_ELEM_NAME || kind == COLL_DOC_NAMED;
}

/* WHICH KINDS ARE §4.2.10.2 HTMLCollectionS, stated POSITIVELY — the traversal and the interface are two
   independent facts about a kind, and reading the second off the first was true only while every descendant
   walk happened to be an HTMLCollection. HTML §3.1.7's getElementsByName is a DESCENDANT walk returning a
   NodeList, so `!coll_is_descendant(kind)` stopped being a test for "this is a NodeList" the moment it
   existed; written the old way, HTMLCollection.prototype.namedItem.call(<that NodeList>, x) would have walked
   it and answered. coll_new asserts this predicate against the interface decl each collection is actually
   built with, so the two cannot drift apart in silence. */
static bool coll_is_htmlcollection(int kind)
{
    return kind == COLL_CHILDREN || kind == COLL_BY_TAG || kind == COLL_BY_TAG_NS ||
           kind == COLL_BY_CLASS || kind == COLL_LINKS || kind == COLL_NAMED || kind == COLL_DOC_NAMED;
}

static JSValue coll_slots(JSContext *ctx, JSValueConst v)
{
    JSAtom k;
    JSValue slots;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, v, k) <= 0)   /* an own SLOT, never a lookup — see event.c */
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

static int coll_kind(JSContext *ctx, JSValueConst v, JSValue *powner)
{
    JSValue slots = coll_slots(ctx, v), k;
    int32_t kind = -1;

    if (powner) *powner = JS_UNDEFINED;
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return -1; }
    k = JS_GetPropertyStr(ctx, slots, "kind");
    JS_ToInt32(ctx, &kind, k);
    JS_FreeValue(ctx, k);
    if (powner) *powner = JS_GetPropertyStr(ctx, slots, "owner");
    JS_FreeValue(ctx, slots);
    return kind;
}

/* DOM §4.9 "Interface Element"'s class matching: the element must carry EVERY token the query asks
   for. Both sides are ASCII-whitespace separated token lists, which is why this is a nested scan and
   not a string compare — `getElementsByClassName('a b')` matches `class="b x a"`. */
static bool coll_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool coll_token_in(const char *hay, size_t hlen, const char *tok, size_t tlen)
{
    size_t i = 0;
    while (i < hlen) {
        size_t st;
        while (i < hlen && coll_is_space(hay[i])) i++;
        st = i;
        while (i < hlen && !coll_is_space(hay[i])) i++;
        if (i - st == tlen && memcmp(hay + st, tok, tlen) == 0) return true;
    }
    return false;
}

static bool coll_has_all_classes(const lxb_dom_element_t *el, const char *q, size_t qlen)
{
    size_t vlen = 0, i = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute((lxb_dom_element_t *)el,
                                                        (const lxb_char_t *)"class", 5, &vlen);
    bool any = false;
    if (!v) return false;
    while (i < qlen) {
        size_t st;
        while (i < qlen && coll_is_space(q[i])) i++;
        st = i;
        while (i < qlen && !coll_is_space(q[i])) i++;
        if (i == st) continue;
        any = true;
        if (!coll_token_in((const char *)v, vlen, q + st, i - st)) return false;
    }
    /* DOM §4.9: an EMPTY token list matches nothing, which is not the same as matching everything. */
    return any;
}

/* ASCII case-insensitive compare — DOM §4.5 "Interface Document"'s rule is ASCII-only, so a
   locale-aware one would be wrong. */
static bool coll_ascii_ieq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

/* Is this node one the collection counts? A NodeList counts every node; an HTMLCollection counts elements; a
   by-name collection counts the elements whose name matches what it was built with. */
/* DOM §4.5 "Interface Document" STATES TWO BY-NAME ALGORITHMS AND THEY TAKE DIFFERENT OPERANDS.
   "list of elements with qualified name qualifiedName" matches ONE string against the qualified
   name, with the HTML-document lowercasing rule.
   "list of elements with namespace namespace and local name localName" matches TWO, against the NAMESPACE and
   the LOCAL name, with `*` meaning "any" in each position INDEPENDENTLY and with no lowercasing anywhere. They
   are not one algorithm with an optional argument, so a query is a record a walk carries rather than a bare
   string — and reading it once per walk rather than once per node is what keeps the constant smaller than the
   algorithm. */
typedef struct {
    const char *name;   /* the qualified name, or the local name for the NS form */
    size_t      nlen;
    const char *ns;     /* the NS form's namespace; NULL means the null namespace, which is a real query */
    size_t      nslen;
} CollQuery;

/* Does this element's namespace match the query's? DOM §4.5: the empty string was already turned into null by
   validate-and-extract's first step, `*` matches any, and null matches an element in NO namespace — which is
   what Lexbor answers with an empty or absent namespace URL for. */
static bool coll_ns_matches(const CollQuery *q, const lxb_dom_node_t *c)
{
    size_t len = 0;
    const lxb_char_t *url;

    if (q->ns && q->nslen == 1 && q->ns[0] == '*') return true;
    url = lxb_ns_by_id(c->owner_document->ns, c->ns, &len);
    if (!q->ns) return url == NULL || len == 0;
    return url && len == q->nslen && memcmp(url, q->ns, len) == 0;
}

/* HTML §7.2.2.3 "Named access on the Window object"' named elements, two rules: any element whose ID is the name,
   and `embed`/`form`/`img`/`object` whose `name` attribute is (a `<div name=x>` is not one, a `<div id=x>` is).
   The first rule is over DOM §4.9's ID ("elements whose ID is name"), never the `id` content attribute, so it is
   element_id_bytes: `setAttributeNS(ns, "id", "x")` gives no ID and `id=""` none either.
   Named residual. Not covered: the `name` half diverges three ways — it also takes `iframe` (an approximation of
   the document-tree child navigable target name set, a rule over a navigable's target name), it does not test
   the "non-empty name content attribute" clause, and it reads the qualified name where the key is (null
   namespace, `name`). Next diff builds the name half as its own predicate over (null namespace, `name`), with
   the navigable set split out of the element list. Absence shows as `window[n]` answering an element for a
   document whose only `n`-named thing is an `iframe` with no content navigable, or a namespaced or empty
   `name` attribute. */
int collections_window_named_names(lxb_dom_element_t *el, const lxb_char_t *v[2], size_t len[2])
{
    size_t vl = 0, qn = 0;
    const lxb_char_t *q, *id = element_id_bytes(el, &vl);
    int k = 0;

    if (id) { v[k] = id; len[k++] = vl; }
    q = lxb_dom_element_qualified_name(el, &qn);
    if (!q) return k;
    if (!((qn == 5 && memcmp(q, "embed", 5) == 0) || (qn == 4 && memcmp(q, "form", 4) == 0) ||
          (qn == 3 && memcmp(q, "img", 3) == 0)   || (qn == 6 && memcmp(q, "object", 6) == 0) ||
          (qn == 6 && memcmp(q, "iframe", 6) == 0)))
        return k;
    vl = 0;
    q = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"name", 4, &vl);
    if (q) { v[k] = q; len[k++] = vl; }
    return k;
}

bool collections_window_named_is(lxb_dom_element_t *el, const char *name, size_t nlen)
{
    const lxb_char_t *v[2];
    size_t len[2];
    int k = collections_window_named_names(el, v, len);

    for (int i = 0; i < k; i++)
        if (len[i] == nlen && memcmp(v[i], name, nlen) == 0) return true;
    return false;
}

static bool coll_takes(int kind, const CollQuery *qy, const lxb_dom_node_t *c)
{
    const char *name = qy ? qy->name : NULL;
    size_t nlen = qy ? qy->nlen : 0;

    if (kind == COLL_CHILD_NODES) return true;
    if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (kind == COLL_BY_TAG) {
        size_t qn = 0;
        const lxb_char_t *q;
        /* DOM §4.5: `*` is every element, and it is the form a page uses to count a subtree. */
        if (nlen == 1 && name[0] == '*') return true;
        q = lxb_dom_element_qualified_name((lxb_dom_element_t *)c, &qn);
        if (!q || qn != nlen) return false;
        /* DOM §4.5: in an HTML document the query is matched against the qualified name LOWERCASED, for elements in
           the HTML namespace. Lexbor already stores those lowercased, so what this has to do is accept an
           uppercase query — `getElementsByTagName('I')` found nothing until it did, and `DIV` is how a great
           deal of older code spells it. A non-HTML element matches exactly, which is the other half of the
           same sentence. */
        if (c->ns == LXB_NS_HTML) return coll_ascii_ieq((const char *)q, name, nlen);
        return memcmp(q, name, nlen) == 0;
    }
    if (kind == COLL_BY_TAG_NS) {
        size_t ln = 0;
        const lxb_char_t *l;

        if (!coll_ns_matches(qy, c)) return false;
        if (nlen == 1 && name[0] == '*') return true;   /* `*` is any LOCAL name, independently of the ns */
        l = lxb_dom_element_local_name((lxb_dom_element_t *)c, &ln);
        /* THE LOCAL NAME, EXACTLY — no HTML lowercasing. `getElementsByTagNameNS` is the case-sensitive form,
           which is the whole reason a page reaches for it over getElementsByTagName in an XML document. */
        return l && ln == nlen && memcmp(l, name, nlen) == 0;
    }
    if (kind == COLL_BY_CLASS)
        return coll_has_all_classes(lxb_dom_interface_element((lxb_dom_node_t *)c), name, nlen);
    if (kind == COLL_NAMED) return collections_window_named_is((lxb_dom_element_t *)c, name, nlen);
    if (kind == COLL_BY_ELEM_NAME) {
        /* HTML §3.1.7 "DOM tree accessors"' getElementsByName, verbatim: "return a live NodeList containing
           all the HTML elements in that document that have a name attribute whose value is identical to the
           elementName argument, in tree order".
           HTML ELEMENTS IS HALF THE FILTER, and the half a by-attribute walk would drop: HTML §2.1.3
           "XML compatibility" defines the term as "any element in that namespace, even in XML
           documents" — the namespace being the HTML one — so `<svg name=x>` and an XML
           `<thing name=x>` are not matches however their attribute reads.
           IDENTICAL IS BYTE EQUALITY. No lowercasing of either side, unlike DOM §4.5's
           qualified-name match one arm up, whose HTML rule is about the TAG and says nothing about
           an attribute VALUE. */
        size_t vl = 0;
        const lxb_char_t *v;
        if (c->ns != LXB_NS_HTML) return false;
        v = lxb_dom_element_get_attribute((lxb_dom_element_t *)c, (const lxb_char_t *)"name", 4, &vl);
        return v && vl == nlen && memcmp(v, name, nlen) == 0;
    }
    if (kind == COLL_LINKS) {
        /* HTML §3.1.7 "DOM tree accessors"' `document.links` is `a` AND `area` elements THAT HAVE
           AN href — the attribute is half the definition, so an anchor used as a scroll target is
           not a link.
           HAVING IT IS `has_attribute`, a question about the attribute list, so `<a href>` is a link. */
        size_t qn = 0;
        const lxb_char_t *q = lxb_dom_element_qualified_name((lxb_dom_element_t *)c, &qn);
        if (!q || !((qn == 1 && q[0] == 'a') || (qn == 4 && memcmp(q, "area", 4) == 0))) return false;
        return lxb_dom_element_has_attribute((lxb_dom_element_t *)c, (const lxb_char_t *)"href", 4);
    }
    if (kind == COLL_DOC_NAMED) return document_named_element_is((lxb_dom_element_t *)c, name, nlen);
    return true;
}

/* THE PRE-ORDER SUCCESSOR AND PREDECESSOR within `root`'s subtree — the traversal a descendant collection uses,
   and it needs BOTH because the index cache steps backwards when a page iterates in reverse. */
static lxb_dom_node_t *coll_pre_next(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    if (n->first_child) return n->first_child;
    while (n != root) {
        if (n->next) return n->next;
        n = n->parent;
        if (!n) return NULL;
    }
    return NULL;
}

static lxb_dom_node_t *coll_pre_prev(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    lxb_dom_node_t *p;
    if (n == root) return NULL;
    if (!n->prev) return n->parent == root ? NULL : n->parent;
    /* the previous sibling's DEEPEST LAST descendant is what pre-order visited just before this node */
    for (p = n->prev; p->last_child; p = p->last_child) { }
    return p;
}

/* The first candidate, and the next/previous one — the only place a kind's traversal is decided. */
static lxb_dom_node_t *coll_first_node(int kind, lxb_dom_node_t *root)
{
    return coll_is_descendant(kind) ? coll_pre_next(root, root) : root->first_child;
}

static lxb_dom_node_t *coll_adv(int kind, lxb_dom_node_t *root, lxb_dom_node_t *n, int forward)
{
    if (coll_is_descendant(kind)) return forward ? coll_pre_next(n, root) : coll_pre_prev(n, root);
    return forward ? n->next : n->prev;
}

/* THE INDEX CACHE, Blink's CollectionIndexCache. A live collection has nothing to cache the tree in, so counting it
   is a walk and reaching index `i` is a walk of `i`; the cache makes the loop every page writes
   (`for (i = 0; i < el.children.length; i++) el.children[i]`) linear rather than quadratic in its markup. Each walk
   rests per node (CollWalk). The cache is a memo, valid exactly while what it read is unchanged: the tree version,
   which the swap advances, so a cache another flow filled is already invalid by the time this one runs, and for a
   kind whose filter reads attributes the attribute generation too, because an attribute write moves no tree
   version. Nothing here is per-flow state; a miss costs the walk. */
typedef struct {
    uint64_t        version;      /* the tree this describes; never 0, because the version starts at 1 */
    uint64_t        attr_gen;     /* the attributes it read (dom_attr_generation), or 0 for a kind reading none */
    lxb_dom_node_t *owner;        /* whose child list — the same object can be re-read from a different flow */
    lxb_dom_node_t *node;         /* the member at `index`, or NULL when only the length is known */
    uint32_t        index;
    uint32_t        length;
    uint8_t         has_length;
} CollCache;

/* Is `n` inside `root`'s subtree — the invariant a cached member has to keep. */
static bool coll_in_subtree(const lxb_dom_node_t *n, const lxb_dom_node_t *root)
{
    for (; n; n = n->parent)
        if (n == root) return true;
    return false;
}

/* The name a by-name collection was built with. Read once per call rather than per node — the walk is the
   node-shaped part, and a CString per node would make the constant bigger than the algorithm. */
static void coll_query(JSContext *ctx, JSValueConst self, int kind, CollQuery *q)
{
    JSValue slots, nv;

    q->name = q->ns = NULL;
    q->nlen = q->nslen = 0;
    if (!coll_is_descendant(kind)) return;
    slots = coll_slots(ctx, self);
    nv = JS_GetPropertyStr(ctx, slots, "name");
    q->name = JS_ToCStringLen(ctx, &q->nlen, nv);
    JS_FreeValue(ctx, nv);
    if (kind == COLL_BY_TAG_NS) {
        /* THE NULL NAMESPACE IS A REAL QUERY, so it is JS_NULL in the slot and NULL here — not the empty
           string, which DOM §4.5's first step has already turned into null and which would otherwise match an
           element whose namespace URL is genuinely empty by accident rather than by the algorithm. */
        JSValue nsv = JS_GetPropertyStr(ctx, slots, "ns");
        if (!JS_IsNull(nsv) && !JS_IsUndefined(nsv))
            q->ns = JS_ToCStringLen(ctx, &q->nslen, nsv);
        JS_FreeValue(ctx, nsv);
    }
    JS_FreeValue(ctx, slots);
}

static void coll_query_free(JSContext *ctx, CollQuery *q)
{
    if (q->name) JS_FreeCString(ctx, q->name);
    if (q->ns)   JS_FreeCString(ctx, q->ns);
    q->name = q->ns = NULL;
}

/* Whether `kind`'s filter reads an attribute (class, href, id or name), so its members change on an attribute
   write; a child kind and a by-tag kind read node types and names, which no write changes. */
static bool coll_filters_attributes(int kind)
{
    return kind == COLL_BY_CLASS || kind == COLL_LINKS || kind == COLL_NAMED || kind == COLL_BY_ELEM_NAME ||
           kind == COLL_DOC_NAMED;
}

/* The cache for this collection, already checked against what it describes. NULL means walk from scratch. */
static CollCache *coll_cache(JSValueConst self, lxb_dom_node_t *owner, int kind)
{
    CollCache *c = idl_indexed_cache(self);
    uint64_t attr_gen = coll_filters_attributes(kind) ? dom_attr_generation() : 0;

    if (!c) return NULL;
    if (c->version != dom_cow_version() || c->owner != owner || c->attr_gen != attr_gen) {
        memset(c, 0, sizeof *c);
        c->version = dom_cow_version();
        c->attr_gen = attr_gen;
        c->owner = owner;
    }
    return c;
}

/* ---- the walks, one node per entry ---------------------------------------------------------------------- */

/* A collection walk's resume point, kept in a step machine's state: the collection's kind and root, the node last
   visited (NULL before the first), and the members passed. Nodes only, so a state holding one needs no visit;
   zeroed means not begun. A walk keeps the kind and root it began with. */
typedef struct {
    lxb_dom_node_t *root, *cursor;
    uint64_t        passed, want;   /* members visited; for an index, how many reach it */
    int             kind;
    uint8_t         begun, forward;
} CollWalk;

static void coll_walk_begin(CollWalk *w, int kind, lxb_dom_node_t *root, lxb_dom_node_t *from, int forward,
                            uint64_t want)
{
    DCHECK(!w->begun && root != NULL && (from != NULL || forward),
           "a collection walk began twice, over no root, or backwards from before its first node");
    w->root = root;
    w->cursor = from;
    w->kind = kind;
    w->forward = (uint8_t)forward;
    w->want = want;
    w->passed = 0;
    w->begun = 1;
}

/* The node after the cursor in the walk's direction, charged one node visited; NULL past the last. */
static lxb_dom_node_t *coll_walk_next(CollWalk *w)
{
    lxb_dom_node_t *c;

    DCHECK(w->begun, "a collection walk was stepped before it began");
    c = w->cursor ? coll_adv(w->kind, w->root, w->cursor, w->forward) : coll_first_node(w->kind, w->root);
    if (!c) return NULL;
    thread_work_charge(THREAD_WORK_DOM_WALK, 1);
    w->cursor = c;
    return c;
}

/* Whether `c` is a member of `self`. The query is read from the slots at each node, because its bytes are the
   slot string's and do not outlive the entry. */
static bool coll_walk_takes(JSContext *ctx, JSValueConst self, const CollWalk *w, const lxb_dom_node_t *c)
{
    CollQuery q;
    bool takes;

    coll_query(ctx, self, w->kind, &q);
    takes = coll_takes(w->kind, &q, c);
    coll_query_free(ctx, &q);
    return takes;
}

/* A static list's length: its array's, which the engine built and no page reaches. */
static uint32_t coll_static_length(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t count = 0;

    JS_ToUint32(ctx, &count, lv);
    JS_FreeValue(ctx, lv);
    return count;
}

/* DOM §4.2.10.1's and §4.2.10.2's `length`, "the number of nodes represented by the collection", one node per
   call: true while the walk has more to visit, so the caller rests; false with `*plen` set. */
static bool coll_length_rest(JSContext *ctx, JSValueConst self, CollWalk *w, uint32_t *plen)
{
    lxb_dom_node_t *c;
    CollCache *cc;

    if (!w->begun) {
        JSValue owner;
        int kind = coll_kind(ctx, self, &owner);
        lxb_dom_node_t *n;

        if (kind == COLL_STATIC) {
            *plen = coll_static_length(ctx, owner);
            JS_FreeValue(ctx, owner);
            return false;
        }
        n = node_of(owner);
        JS_FreeValue(ctx, owner);
        *plen = 0;
        if (!n) return false;
        cc = coll_cache(self, n, kind);
        if (cc && cc->has_length) { *plen = cc->length; return false; }
        /* THE TREE AS IT IS NOW. That walk is the liveness — there is nothing here to invalidate or refresh,
           because the version says when what was counted stopped describing the tree. */
        coll_walk_begin(w, kind, n, NULL, 1, 0);
    }
    c = coll_walk_next(w);
    if (c) {
        if (coll_walk_takes(ctx, self, w, c)) w->passed++;
        return true;
    }
    DCHECK(w->passed <= UINT32_MAX, "a collection counted more members than a tree can hold");
    cc = coll_cache(self, w->root, w->kind);
    if (cc) { cc->length = (uint32_t)w->passed; cc->has_length = 1; }
    *plen = (uint32_t)w->passed;
    return false;
}

/* DOM §4.2.10's item and indexed getter, the indexth node in the collection, one node per call: true while the
   walk has more to visit; false with `*pres` the node (owned), or JS_UNDEFINED past the end. A loop that reads 0,
   1, 2… moves one member per call from the cached one rather than i of them, and a read that goes backwards walks
   through `prev`. */
static bool coll_item_rest(JSContext *ctx, JSValueConst self, uint32_t i, CollWalk *w, JSValue *pres)
{
    lxb_dom_node_t *c;
    CollCache *cc;

    *pres = JS_UNDEFINED;
    if (!w->begun) {
        JSValue owner;
        int kind = coll_kind(ctx, self, &owner);
        lxb_dom_node_t *n;

        if (kind == COLL_STATIC) {
            *pres = JS_GetPropertyUint32(ctx, owner, i);
            JS_FreeValue(ctx, owner);
            return false;
        }
        n = node_of(owner);
        JS_FreeValue(ctx, owner);
        if (!n) return false;
        cc = coll_cache(self, n, kind);
        if (cc && cc->has_length && i >= cc->length) return false;
        if (cc && cc->node) {
            /* THE CACHED MEMBER IS STILL IN THE COLLECTION'S SUBTREE. For the child kinds that is one pointer; for
               a descendant kind it is a walk up to the owner, which is O(depth) and dev-only. Writing it as
               `parent == n` was right for the two kinds that existed and silently wrong for the two that did not —
               a descendant is not a child, and the assert fired on the first by-name collection built. */
            DCHECK(coll_in_subtree(cc->node, n),
                   "a cached collection member has left the collection's owner — a tree mutation did not advance "
                   "dom_cow_version, so the cache is describing a tree that is gone");
            if (i == cc->index) { *pres = node_wrap(ctx, cc->node); return false; }
            coll_walk_begin(w, kind, n, cc->node, i > cc->index, i > cc->index ? i - cc->index : cc->index - i);
        } else {
            coll_walk_begin(w, kind, n, NULL, 1, (uint64_t)i + 1);
        }
    }
    c = coll_walk_next(w);
    if (!c) return false;
    if (!coll_walk_takes(ctx, self, w, c) || ++w->passed < w->want) return true;
    cc = coll_cache(self, w->root, w->kind);
    if (cc) { cc->index = i; cc->node = c; }
    *pres = node_wrap(ctx, c);
    return false;
}

/* §4.2.10.2 Interface HTMLCollection's named getter and namedItem(key): the first element in the collection whose
   ID is `name`, or which is in the HTML namespace with a `name` attribute that is. Only HTMLCollection has one; a
   NodeList's IDL declares none, so `nodes.foo` is honestly undefined. One node per call: true while the walk has
   more to visit; false with `*pres` the element (owned) or JS_UNDEFINED. */
static bool coll_named_rest(JSContext *ctx, JSValueConst self, const char *name, size_t nlen, CollWalk *w,
                            JSValue *pres)
{
    lxb_dom_node_t *c;
    const lxb_char_t *v;
    size_t vlen = 0;

    *pres = JS_UNDEFINED;
    if (!w->begun) {
        JSValue owner;
        int kind = coll_kind(ctx, self, &owner);
        lxb_dom_node_t *n = node_of(owner);

        JS_FreeValue(ctx, owner);
        /* §4.2.10.2's named getter belongs to HTMLCollection AND TO NO OTHER KIND, which is why the test names
           those kinds rather than excluding the NodeLists: `kind != COLL_CHILDREN && !coll_is_descendant(kind)`
           said the same thing only while every descendant walk was an HTMLCollection, and getElementsByName is a
           descendant walk that is not one. A foreign receiver reaches here with kind -1 and is excluded by the
           same line — which is why this is a refusal and not a DCHECK: `this` is page-supplied, and
           HTMLCollection.prototype.namedItem.call({}, "x") is a page's input rather than a broken invariant.
           NAMED RESIDUAL — a NodeList reaching this member answers `null` where Web IDL §3.7.7 Operations
           requires a TypeError for a receiver that does not implement HTMLCollection. WHAT THE NEXT DIFF BUILDS:
           an `idl_this_iface` declaration on `namedItem` naming a `collections_is_htmlcollection` predicate this
           component would export, which is the same shape core/dom/document.c already declares for its own
           members. HOW ITS ABSENCE WOULD SHOW: calling that member with any receiver the corpus can reach — a
           NodeList, a plain object, a DOMTokenList — and observing a value where a browser throws. */
        if (!n || !coll_is_htmlcollection(kind)) return false;
        /* §4.2.10.2 STEP 1, which had no spelling here at all: "The namedItem(key) method steps are: If key is
           the empty string, return null." IT IS NOT REDUNDANT WITH §4.9'S UNSET STEP, which is the reading that
           would leave it out — no element's ID is the empty string, so the ID clause below could never answer an
           empty key, and the `name` clause can: lexbor hands back a `name=""` attribute as non-NULL data of length
           0, so `vlen == nlen` held at zero and `hc.namedItem("")` ANSWERED THAT ELEMENT. One step, two clauses,
           and only one of them was already covered.
           JS_UNDEFINED IS §4.2.10.2'S `null` HERE AND ALSO THE RIGHT ANSWER AT THE OTHER DOOR, which is why this
           is one test and not two: js_coll_named_item maps JS_UNDEFINED to JS_NULL for the METHOD, and the named
           getter needs the empty string NOT to be a supported property name — which is the same sentence's own
           supported-property-names algorithm, whose `name` clause appends a value "neither the empty string nor
           is in result". */
        if (nlen == 0) return false;
        coll_walk_begin(w, kind, n, NULL, 1, 0);
    }
    c = coll_walk_next(w);
    if (!c) return false;
    /* Step 2's "the first element in the collection": a member, not any element of the walk's subtree. */
    if (c->type != LXB_DOM_NODE_TYPE_ELEMENT || !coll_walk_takes(ctx, self, w, c)) return true;
    /* §4.2.10.2 step 2's FIRST clause, "it has an ID which is key" — DOM §4.9's ID and not the `id`
       attribute, which is why this is element_id_bytes. A qualified-name read stood here and matched an
       `id` in a non-null namespace, so `hc.namedItem(k)` and `hc[k]` answered an element whose ID §4.9's
       change steps never set. The unset half rides along: no element's ID is the empty string. */
    v = element_id_bytes(lxb_dom_interface_element(c), &vlen);
    if (v && vlen == nlen && memcmp(v, name, nlen) == 0) { *pres = node_wrap(ctx, c); return false; }
    /* §4.2.10.2 step 2's SECOND clause, whose namespace half was missing: "it is in the HTML namespace and
       has a name attribute whose value is key". The ELEMENT's namespace, not the attribute's — so an
       `<svg>` child with `name="x"` is not a match however its attribute reads, and `hc.x` answered it.
       It is the same term HTML §2.1.3 "XML compatibility" defines for getElementsByName one arm up in
       coll_takes, which is why that walk already tests `c->ns != LXB_NS_HTML` and this one did not.
       NAMED RESIDUAL — THE ATTRIBUTE'S OWN NAMESPACE IS STILL UNTESTED. WHAT IS NOT COVERED: "a name
       attribute" is §4.9's three-part definition (local name `name`, namespace null, namespace prefix
       null), and this is a QUALIFIED-NAME read, so an HTML element carrying
       `setAttributeNS(ns, "name", key)` matches where a browser does not — the same defect §4.9's ID half
       above was just routed out of, one attribute over. WHAT THE NEXT DIFF BUILDS: this read at
       `dom_attr_get_ns(el, NULL, "name")`, which is the population every `"name", 4` reader in this
       component shares and is therefore its own subproblem rather than a line here. HOW ITS ABSENCE WOULD
       SHOW, as an observation: `hc.namedItem(k)` or `hc[k]` answering an HTML element whose only
       `name`-named attribute is namespaced. */
    if (c->ns != LXB_NS_HTML) return true;
    v = lxb_dom_element_get_attribute(lxb_dom_interface_element(c), (const lxb_char_t *)"name", 4, &vlen);
    if (v && vlen == nlen && memcmp(v, name, nlen) == 0) { *pres = node_wrap(ctx, c); return false; }
    return true;
}

/* ---- the C entries --------------------------------------------------------------------------------------
   Web IDL §3.9's internal methods asked from C (own keys, [[Delete]], a C [[GetOwnProperty]] or [[Set]]): a
   static list and a valid cache answer in place. A walk rests per node on the keyed entry and the members, so a
   C caller that needs one is a caller to route there. */
#define COLL_C_ROUTE "route this caller's read through the keyed entry (get_own_property_step), never a C walk"

static uint32_t coll_length_c(JSContext *ctx, JSValueConst self)
{
    JSValue owner;
    int kind = coll_kind(ctx, self, &owner);
    lxb_dom_node_t *n;
    uint32_t count = 0;
    CollCache *cc;

    if (kind == COLL_STATIC) count = coll_static_length(ctx, owner);
    n = kind == COLL_STATIC ? NULL : node_of(owner);
    JS_FreeValue(ctx, owner);
    if (!n) return count;
    cc = coll_cache(self, n, kind);
    if (cc && cc->has_length) return cc->length;
    DFAIL("a C activation asked a live collection's length with no count cached: the count rests per node as "
          "coll_length_rest, so " COLL_C_ROUTE);
    return 0;
}

static JSValue coll_item_c(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue owner, r = JS_UNDEFINED;
    int kind = coll_kind(ctx, self, &owner);
    lxb_dom_node_t *n;
    CollCache *cc;

    if (kind == COLL_STATIC) r = JS_GetPropertyUint32(ctx, owner, i);
    n = kind == COLL_STATIC ? NULL : node_of(owner);
    JS_FreeValue(ctx, owner);
    if (!n) return r;
    cc = coll_cache(self, n, kind);
    if (cc && cc->has_length && i >= cc->length) return JS_UNDEFINED;
    if (cc && cc->node && cc->index == i) return node_wrap(ctx, cc->node);
    DFAIL("a C activation asked a live collection's index with no member cached there: the walk rests per node "
          "as coll_item_rest, so " COLL_C_ROUTE);
    return JS_UNDEFINED;
}

static JSValue coll_named_c(JSContext *ctx, JSValueConst self, const char *name)
{
    (void)ctx; (void)self;
    if (!name[0]) return JS_UNDEFINED;   /* §4.2.10.2 step 1: the empty string is no supported property name */
    DFAIL("a C activation asked an HTMLCollection's named [[GetOwnProperty]]: the lookup rests per node as "
          "coll_named_rest, so " COLL_C_ROUTE " (a [[Set]] reaching here asks what Web IDL §3.9.2 [[Set]] "
          "step 2's LegacyPlatformObjectGetOwnProperty(O, P, true) ignores)");
    return JS_UNDEFINED;
}

/* ---- the keyed read as a step machine ------------------------------------------------------------------- */

#define COLL_KEYED_ALGORITHM "Web IDL §3.9.7 Abstract operations: LegacyPlatformObjectGetOwnProperty of a collection"
#define COLL_KEYED_STAGES(X) X(COLL_KEYED_WALK, COLL_KEYED_ALGORITHM)
enum { IDL_STEP_STAGE_BASE(COLL_KEYED_STAGES) COLL_KEYED_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const COLL_KEYED_STEPS[] = { COLL_KEYED_STAGES(JS_STEP_STAGE_LABEL) NULL };
static int g_keyed_id = -1;

static void coll_walk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

/* f(obj, key, receiver, op), op 0 for [[Get]], 1 for [[HasProperty]] and 2 for [[GetOwnProperty]]: one node per
   entry, then the indexed or named property's value (true for op 1), or undefined (false) for a miss, which
   quickjs walks on to the prototype. */
static int coll_keyed_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CollWalk *w = st;
    JSAtom key;
    uint32_t i;
    bool more, index;
    JSValue v;
    int op;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == COLL_KEYED_WALK && argc == 4 && JS_IsString(argv[1]) &&
               JS_VALUE_GET_TAG(argv[3]) == JS_TAG_INT,
           "a collection's keyed read was called with a shape the keyed entry does not issue");
    op = JS_VALUE_GET_INT(argv[3]);
    DCHECK(op == 0 || op == 1 || op == 2,
           "a collection's keyed read was asked neither [[Get]], [[HasProperty]] nor [[GetOwnProperty]]");
    key = JS_ValueToAtom(ctx, argv[1]);
    if (key == JS_ATOM_NULL) return JS_STEP_ABRUPT;
    index = JS_AtomIsIndex(ctx, &i, key);
    JS_FreeAtom(ctx, key);
    if (index) {
        more = coll_item_rest(ctx, argv[0], i, w, &v);
    } else {
        size_t n;
        const char *name = JS_ToCStringLen(ctx, &n, argv[1]);

        if (!name) return JS_STEP_ABRUPT;
        more = coll_named_rest(ctx, argv[0], name, n, w, &v);
        JS_FreeCString(ctx, name);
    }
    if (more) return JS_STEP_YIELD;
    if (op == 1) {
        *presult = JS_NewBool(ctx, !JS_IsUndefined(v));
        JS_FreeValue(ctx, v);
        return JS_STEP_DONE;
    }
    if (op == 2 && !index && !JS_IsUndefined(v))
        DFAIL("an HTMLCollection's named [[GetOwnProperty]] hit answers the indexed descriptor the class's "
              "get_own_property_step_flags state, where [LegacyUnenumerableNamedProperties] makes a named one "
              "not enumerable: build a per-answer descriptor on the step's op 2");
    *presult = v;
    return JS_STEP_DONE;
}

static const IdlStepDecl COLL_KEYED_DECL = {
    coll_keyed_step, sizeof(CollWalk), coll_walk_visit, NULL, COLL_KEYED_ALGORITHM, COLL_KEYED_STEPS
};

/* ---- the key enumeration as a step machine -------------------------------------------------------------- */

#define COLL_KEYS_ALGORITHM "Web IDL §3.9.6 [[OwnPropertyKeys]] of a collection"
#define COLL_KEYS_STAGES(X) X(COLL_KEYS_INDICES, COLL_KEYS_ALGORITHM " step 2 (the supported property indices)")
enum { IDL_STEP_STAGE_BASE(COLL_KEYS_STAGES) COLL_KEYS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const COLL_KEYS_STEPS[] = { COLL_KEYS_STAGES(JS_STEP_STAGE_LABEL) NULL };
static int g_keys_id = -1;

/* The enumeration's state: the count's walk, then the Array of index strings filled one index per entry. */
typedef struct {
    CollWalk walk;
    JSValue  keys;     /* owned once `started` */
    uint32_t length, filled;
    uint8_t  started, counted;
} CollKeysState;

static void coll_keys_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CollKeysState *s = st;

    if (s->started) v->val(ctx, &s->keys);
}

/* f(obj, undefined, undefined, 4): step 2's indices, "0" to length - 1 in ascending order, counted one node per
   entry and appended one index per entry. Steps 3-5 are not this machine's: the keyed entry appends the
   object's ordinary own keys, and the named properties are idl_indexed.c's residual. */
static int coll_keys_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CollKeysState *s = st;
    char buf[16];

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == COLL_KEYS_INDICES && argc == 4 && JS_VALUE_GET_TAG(argv[3]) == JS_TAG_INT &&
               JS_VALUE_GET_INT(argv[3]) == 4,
           "a collection's key enumeration was called with a shape the keyed entry does not issue");
    if (!s->started) {
        s->keys = JS_NewArray(ctx);
        if (JS_IsException(s->keys)) { s->keys = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        s->started = 1;
    }
    if (!s->counted) {
        if (coll_length_rest(ctx, argv[0], &s->walk, &s->length)) return JS_STEP_YIELD;
        s->counted = 1;
    }
    if (s->filled < s->length) {
        snprintf(buf, sizeof buf, "%u", (unsigned)s->filled);
        if (JS_DefinePropertyValueUint32(ctx, s->keys, s->filled, JS_NewString(ctx, buf),
                                         JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            return JS_STEP_ABRUPT;
        s->filled++;
        return JS_STEP_YIELD;
    }
    *presult = s->keys;
    s->keys = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl COLL_KEYS_DECL = {
    coll_keys_step, sizeof(CollKeysState), coll_keys_visit, NULL, COLL_KEYS_ALGORITHM, COLL_KEYS_STEPS
};

/* IdlIndexedDecl.own_names_step: the enumeration of a live collection; JS_UNDEFINED for a static list, which
   the C entry answers from its array. Owned. */
static JSValue coll_own_names_step(JSContext *ctx, JSValueConst self)
{
    if (coll_kind(ctx, self, NULL) == COLL_STATIC) return JS_UNDEFINED;
    DCHECK(g_keys_id >= 0, "a collection's key enumeration asked for its step before collections_init declared it");
    return idl_step_function(ctx, "collection key enumeration", g_keys_id);
}

/* IdlIndexedDecl.own_step for NodeList: the walk for a live list's index; JS_UNDEFINED for a static list or any
   other key, which the C entries answer. Owned. */
static JSValue nodelist_own_step(JSContext *ctx, JSValueConst self, JSAtom prop)
{
    uint32_t i;

    if (!JS_AtomIsIndex(ctx, &i, prop) || coll_kind(ctx, self, NULL) == COLL_STATIC) return JS_UNDEFINED;
    DCHECK(g_keyed_id >= 0, "a collection's keyed read asked for its step before collections_init declared it");
    return idl_step_function(ctx, "NodeList keyed read", g_keyed_id);
}

/* IdlIndexedDecl.own_step for HTMLCollection, which is always live: the walk for an index or a String key;
   JS_UNDEFINED for a Symbol, which names no property of either kind. Owned. */
static JSValue htmlcoll_own_step(JSContext *ctx, JSValueConst self, JSAtom prop)
{
    uint32_t i;

    (void)self;
    if (!JS_AtomIsIndex(ctx, &i, prop)) {
        JSValue pv = JS_AtomToValue(ctx, prop);
        bool str = JS_IsString(pv);

        JS_FreeValue(ctx, pv);
        if (!str) return JS_UNDEFINED;
    }
    DCHECK(g_keyed_id >= 0, "a collection's keyed read asked for its step before collections_init declared it");
    return idl_step_function(ctx, "HTMLCollection keyed read", g_keyed_id);
}

/* A LIVE collection carries the index cache; the STATIC one does not, because its item() is already an array
   read and a cache would be a second copy of the answer to keep in step. Both kinds share one decl per
   interface, so the live decl's cache_size is what a static NodeList allocates too — a few bytes it never
   reads, against a second decl that differs in one field. */
static const IdlIndexedDecl NODELIST_INDEXED = { "NodeList", coll_length_c, coll_item_c, NULL, sizeof(CollCache),
                                                 nodelist_own_step, coll_own_names_step };
static const IdlIndexedDecl HTMLCOLL_INDEXED = { "HTMLCollection", coll_length_c, coll_item_c, coll_named_c,
                                                 sizeof(CollCache), htmlcoll_own_step, coll_own_names_step };

/* ---- the members ------------------------------------------------------------------------------------------ */

#define COLL_LENGTH_ALGORITHM "DOM §4.2.10 Old-style collections: NodeList and HTMLCollection length"
#define COLL_LENGTH_STAGES(X) \
    X(COLL_LENGTH_COUNT, COLL_LENGTH_ALGORITHM " (return the number of nodes represented by the collection)")
enum { IDL_STEP_STAGE_BASE(COLL_LENGTH_STAGES) COLL_LENGTH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const COLL_LENGTH_STEPS[] = { COLL_LENGTH_STAGES(JS_STEP_STAGE_LABEL) NULL };
static int g_length_id = -1;

static int js_coll_length(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    uint32_t len;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == COLL_LENGTH_COUNT, "a collection's length resumed into a stage it does not have");
    if (coll_length_rest(ctx, hdr->this_val, st, &len)) return JS_STEP_YIELD;
    *presult = JS_NewUint32(ctx, len);
    return JS_STEP_DONE;
}

static const IdlStepDecl COLL_LENGTH_DECL = {
    js_coll_length, sizeof(CollWalk), coll_walk_visit, NULL, COLL_LENGTH_ALGORITHM, COLL_LENGTH_STEPS
};

/* §4.2.10.1 Interface NodeList's item(index) — "The item(index) method must return the indexth node in the
   collection. If there is no indexth node in the collection, then the method must return null." That null past
   the end is what makes it different from the indexed getter, whose supported property indices stop at the
   length. §4.2.10.2 Interface HTMLCollection states the same operation over elements.

   THE INDEX IS `unsigned long`, AND THE BODY NO LONGER RE-STATES THAT. This read used to be `JS_ToInt64` plus
   `if (i < 0) return JS_NULL`, under a declaration of `long` — a signed type Web IDL §3.2.4.5 long converts
   with Web IDL §3.2.4.9 Abstract operations' ConvertToInt(V, 32, "signed"), whose final step is "If signedness is
   'signed' and x ≥ 2^(bitLength−1), then return x − 2^bitLength". So `item(2**31)` denoted −2147483648 where
   §3.2.4.6 unsigned long's ConvertToInt(V, 32, "unsigned") denotes 2147483648, and the body's negative branch
   is what turned that back into the null a browser answers. THE COMPENSATION IS WHY THE WRONG TYPE SURVIVED: it
   made the declaration's error unobservable through this member (a collection cannot hold 2^31 nodes, so every
   value ≥ 2^31 is past the end under either sign), so nothing ever fired. The declaration is the spec of the
   conversion; a body re-deriving the sign is the second copy of Web IDL §3.2.4.9's arithmetic that idl_args.c exists to
   prevent. */
/* IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN. The index reaches this body unconverted
 * because Web IDL §3.2 "JavaScript type mapping"'s conversion is a boundary unknown external
 * input crosses AS ITSELF (idl_concolic_rule answers
 * IDL_CONCOLIC_CROSSES for every integer type), and reading it with a raw numeric coercion — which is what
 * stood here — is the shape core/idl_args.h bans by name: a concolic is an object, so ToNumber reaches
 * ToPrimitive from a plain C frame and the engine aborts INSIDE the coercion rather than at this member. The
 * fork is core/idl_index_arg.h's elimination chain, and asking it needs a state to snapshot.
 *
 * ONE MACHINE FOR BOTH INTERFACES, exactly as there was one plain body: §4.2.10.1's and §4.2.10.2's `item` are
 * the same operation over the same conversion, and the declaration below is installed on both prototypes. The
 * chain's key names the ALGORITHM and the NUMBER, so a flow that answers `index == 3` at a NodeList has stated
 * a fact about the value and not about that collection's contents. */
#define COLL_ITEM_ALGORITHM "DOM §4.2.10 Old-style collections: NodeList and HTMLCollection item(index)"
#define COLL_ITEM_STAGES(X)                                                                                   \
    X(COLL_ITEM_COUNT, COLL_ITEM_ALGORITHM " (the number of nodes represented by the collection)")            \
    X(COLL_ITEM_READ, COLL_ITEM_ALGORITHM " (return the indexth node or element in the collection, or null)")
enum { IDL_STEP_STAGE_BASE(COLL_ITEM_STAGES) COLL_ITEM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const COLL_ITEM_STEPS[] = { COLL_ITEM_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* item()'s state: the unknown index's chain, its count of positions, the index once known, and the walk. */
typedef struct {
    IdlIndexChain chain;
    CollWalk      walk;
    uint32_t      length, index;
    uint8_t       known;
} CollItemState;

static void coll_item_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    idl_index_chain_visit(ctx, &((CollItemState *)st)->chain, v);
}

static int js_coll_item(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CollItemState *s = state;
    bool past_end = false;
    JSValue r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(argc == 1,
           "§4.2.10's `item` reached its body with an argument count its declaration does not produce — its "
           "one `unsigned long index` is required, so Web IDL §3.6's argument-count check refuses a shorter call");
    if (hdr->stage == COLL_ITEM_COUNT) {
        /* Only an unknown index asks the count: the chain's positions are the collection's indices. */
        if (concolic_is(argv[0]) && coll_length_rest(ctx, hdr->this_val, &s->walk, &s->length))
            return JS_STEP_YIELD;
        memset(&s->walk, 0, sizeof s->walk);
        hdr->stage = COLL_ITEM_READ;
    }
    DCHECK(hdr->stage == COLL_ITEM_READ, "§4.2.10's `item` resumed into a stage the algorithm does not have");
    if (!s->known && concolic_is(argv[0])) {
        int rc = idl_index_chain_run(ctx, hdr, &s->chain, argv[0], s->length, COLL_ITEM_ALGORITHM, &s->index,
                                     &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {
            *presult = JS_NULL;   /* §4.2.10.1's and §4.2.10.2's own past-the-end answer */
            return JS_STEP_DONE;
        }
        s->known = 1;
    } else if (!s->known) {
        s->index = idl_index_arg_known(ctx, argv[0], COLL_ITEM_ALGORITHM);
        s->known = 1;
    }
    if (coll_item_rest(ctx, hdr->this_val, s->index, &s->walk, &r)) return JS_STEP_YIELD;
    *presult = JS_IsUndefined(r) ? JS_NULL : r;
    return JS_STEP_DONE;
}

static const IdlStepDecl COLL_ITEM_DECL = {
    js_coll_item, sizeof(CollItemState), coll_item_visit, NULL,
    COLL_ITEM_ALGORITHM, COLL_ITEM_STEPS, 0, NULL
};

#define COLL_NAMED_ALGORITHM "DOM §4.2.10.2 Interface HTMLCollection: namedItem(key)"
#define COLL_NAMED_STAGES(X) X(COLL_NAMED_WALK, COLL_NAMED_ALGORITHM " step 2 (the first element in the collection)")
enum { IDL_STEP_STAGE_BASE(COLL_NAMED_STAGES) COLL_NAMED_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const COLL_NAMED_STEPS[] = { COLL_NAMED_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_coll_named_item(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    const char *name;
    size_t n;
    bool more;
    JSValue r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == COLL_NAMED_WALK && argc == 1,
           "§4.2.10.2's `namedItem` reached its body in a stage or with an argument count it does not have");
    if (concolic_is(argv[0]))
        DFAIL("§4.2.10.2's namedItem was handed an unknown key: build the walk's fork over the key's domain, the "
              "question the keyed entry's GP_KEYDOMAIN asks of key_domain_step");
    name = JS_ToCStringLen(ctx, &n, argv[0]);   /* a real string by now */
    if (!name) return JS_STEP_ABRUPT;
    more = coll_named_rest(ctx, hdr->this_val, name, n, st, &r);
    JS_FreeCString(ctx, name);
    if (more) return JS_STEP_YIELD;
    *presult = JS_IsUndefined(r) ? JS_NULL : r;
    return JS_STEP_DONE;
}

static const IdlStepDecl COLL_NAMED_DECL = {
    js_coll_named_item, sizeof(CollWalk), coll_walk_visit, NULL, COLL_NAMED_ALGORITHM, COLL_NAMED_STEPS
};

/* ---- construction ------------------------------------------------------------------------------------------ */
static JSValue coll_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl, int kind,
                        JSValueConst owner, const char *name, const char *ns)
{
    JSValue obj, slots;
    JSAtom k;

    /* THE KIND AND THE INTERFACE ARE ONE FACT ASSERTED IN TWO PLACES, so they are compared where both are in
       hand — coll_named keys §4.2.10.2's named getter on the KIND while the dispatcher reaches it through the
       DECL, and a kind added on one side and not the other is a divergence no reader of either site can see.
       Both operands are this engine's own (a builder picks them together), never a page's, which is what
       makes this a DCHECK rather than a refusal. */
    DCHECK((decl == &HTMLCOLL_INDEXED) == coll_is_htmlcollection(kind),
           "a collection's kind and its interface disagree — coll_is_htmlcollection must name exactly the "
           "kinds built with HTMLCOLL_INDEXED, or §4.2.10.2's named getter answers for a NodeList");
    obj = idl_indexed_new(ctx, proto, decl);
    if (JS_IsException(obj)) return obj;
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "collections: OOM allocating a collection's slots");
    JS_SetPropertyStr(ctx, slots, "kind", JS_NewInt32(ctx, kind));
    JS_SetPropertyStr(ctx, slots, "owner", JS_DupValue(ctx, owner));
    if (name) JS_SetPropertyStr(ctx, slots, "name", JS_NewString(ctx, name));
    /* §4.5's NS form: the namespace is part of what the collection IS, so it rides the slots beside the name.
       JS_NULL rather than absent, because the NULL NAMESPACE is a real query and "no namespace was given" is
       not a state this algorithm has. */
    if (kind == COLL_BY_TAG_NS)
        JS_SetPropertyStr(ctx, slots, "ns", ns ? JS_NewString(ctx, ns) : JS_NULL);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the collection slot key could not be reached");
    JS_DefinePropertyValue(ctx, obj, k, slots, 0);
    JS_FreeAtom(ctx, k);
    return obj;
}

/* The [SameObject] cache, on the OWNER's wrapper so it is per-flow like the wrapper. Two kinds live on one
   owner, so the slot is keyed by kind. */
static JSValue coll_cached(JSContext *ctx, JSValueConst owner, int kind, JSValueConst proto,
                           const IdlIndexedDecl *decl)
{
    JSAtom k;
    JSValue cache, coll;

    DCHECK(g_ready, "a collection was asked for before collections_init ran");
    if (!JS_IsObject(owner)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_cache_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &cache, owner, k) <= 0) cache = JS_UNDEFINED;
    if (!JS_IsObject(cache)) {
        JS_FreeValue(ctx, cache);
        cache = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(cache), "collections: OOM allocating the [SameObject] cache");
        JS_DefinePropertyValue(ctx, (JSValue)owner, k, JS_DupValue(ctx, cache), 0);
    }
    JS_FreeAtom(ctx, k);
    coll = JS_GetPropertyUint32(ctx, cache, (uint32_t)kind);
    if (!JS_IsObject(coll)) {
        JS_FreeValue(ctx, coll);
        coll = coll_new(ctx, proto, decl, kind, owner, NULL, NULL);
        JS_SetPropertyUint32(ctx, cache, (uint32_t)kind, JS_DupValue(ctx, coll));
    }
    JS_FreeValue(ctx, cache);
    return coll;
}

JSValue collections_child_nodes(JSContext *ctx, JSValueConst owner)
{
    {
        JSValue proto = nodelist_proto(ctx);
        JSValue r = coll_cached(ctx, owner, COLL_CHILD_NODES, proto, &NODELIST_INDEXED);
        JS_FreeValue(ctx, proto);
        return r;
    }
}

JSValue collections_children(JSContext *ctx, JSValueConst owner)
{
    {
        JSValue proto = htmlcollection_proto(ctx);
        JSValue r = coll_cached(ctx, owner, COLL_CHILDREN, proto, &HTMLCOLL_INDEXED);
        JS_FreeValue(ctx, proto);
        return r;
    }
}

/* DOM §4.5 "Interface Document" and §4.9 "Interface Element" getElementsByTagName /
   getElementsByClassName — LIVE, and not [SameObject]: the spec returns a new
   collection per call (unlike childNodes), because the query is part of what the collection IS. */
JSValue collections_by_name(JSContext *ctx, JSValueConst owner, const char *name, bool by_class)
{
    DCHECK(g_ready, "a by-name collection was built before collections_init ran");
    return coll_new_hc(ctx, by_class ? COLL_BY_CLASS : COLL_BY_TAG, owner, name, NULL);
}

/* DOM §4.5's "list of elements with namespace namespace and local name localName" — getElementsByTagNameNS on both
   Document and Element. LIVE and not [SameObject], like its qualified-name sibling. `ns` NULL is the NULL
   NAMESPACE, which matches an element in no namespace; the empty string became null at the member. */
JSValue collections_by_tag_ns(JSContext *ctx, JSValueConst owner, const char *ns, const char *local)
{
    DCHECK(g_ready, "a by-namespace collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_BY_TAG_NS, owner, local, ns);
}

/* HTML §3.1.7's `document.links` — `a`/`area` WITH an href, which is a predicate rather than a name, so it is its
   own kind rather than a by-tag collection that would also count the anchors with no href. */
JSValue collections_named(JSContext *ctx, JSValueConst owner, const char *name)
{
    DCHECK(g_ready, "a named collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_NAMED, owner, name, NULL);
}

JSValue collections_document_named(JSContext *ctx, JSValueConst owner, const char *name)
{
    DCHECK(g_ready, "a document-named collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_DOC_NAMED, owner, name, NULL);
}

JSValue collections_links(JSContext *ctx, JSValueConst owner)
{
    DCHECK(g_ready, "a links collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_LINKS, owner, NULL, NULL);
}

/* HTML §3.1.7 "DOM tree accessors"' getElementsByName — a descendant walk like the four above it and a
   NodeList unlike any of them, which is what its IDL line says it returns and is the whole reason this does
   not go through coll_new_hc. NOT [SameObject] and not cached: the member's own text makes reusing the
   earlier object a MAY and a fresh NodeList a MUST in every other case, so returning a new one is the
   answer that is conformant for both arms. */
JSValue collections_by_element_name(JSContext *ctx, JSValueConst owner, const char *name)
{
    JSValue proto, r;

    DCHECK(g_ready, "a by-element-name collection was built before collections_init ran");
    proto = nodelist_proto(ctx);
    r = coll_new(ctx, proto, &NODELIST_INDEXED, COLL_BY_ELEM_NAME, owner, name, NULL);
    JS_FreeValue(ctx, proto);
    return r;
}

JSValue collections_static(JSContext *ctx, JSValue nodes)
{
    JSValue coll;

    DCHECK(g_ready, "a static NodeList was built before collections_init ran");
    {
        JSValue proto = nodelist_proto(ctx);
        coll = coll_new(ctx, proto, &NODELIST_INDEXED, COLL_STATIC, nodes, NULL, NULL);
        JS_FreeValue(ctx, proto);
    }
    JS_FreeValue(ctx, nodes);   /* the slots hold their own reference */
    return coll;
}

void collections_init(JSContext *ctx)
{
    /* §4.2.10.1 and §4.2.10.2 both write `getter Node? item(unsigned long index)` — NEITHER carries
       [EnforceRange], so Web IDL §3.2.4.9 Abstract operations' ConvertToInt modulo IS the specified behaviour and there
       is nothing here to throw. The type states the SIGN, which is the whole of what it decides. */
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
    static const IdlArgType KEYED_ARGS[4] = { IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY };
    JSClassDef nl = { "NodeList" }, hc = { "HTMLCollection" };

    DCHECK(!g_ready, "collections_init ran twice — the interfaces are declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "collectionSlots", false);
    g_cache_key = JS_NewSymbol(ctx, "collectionCache", false);
    CHECK(!JS_IsException(g_key) && !JS_IsException(g_cache_key),
          "the collection slot keys could not be allocated");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nodelist_class);
    JS_NewClass(JS_GetRuntime(ctx), g_nodelist_class, &nl);
    agent_state_class("element", &g_nodelist_class, "DOM §4.2.10.1 \"Interface NodeList\"'s class");
    JS_NewClassID(JS_GetRuntime(ctx), &g_htmlcoll_class);
    JS_NewClass(JS_GetRuntime(ctx), g_htmlcoll_class, &hc);
    agent_state_class("element", &g_htmlcoll_class, "DOM §4.2.10.2 \"Interface HTMLCollection\"'s class");
    /* ONE declaration for `item`, installed on BOTH prototypes — it is the same operation with the same
       conversion in both IDLs, and two pool entries would be two copies of one fact. */
    g_item_id = idl_method_id_step(ctx, ONE_ULONG, 1, NULL, 0, &COLL_ITEM_DECL, 0);
    g_named_item_id = idl_method_id_step(ctx, ONE_STR, 1, NULL, 0, &COLL_NAMED_DECL, 0);
    g_length_id = idl_getter_id_step(ctx, &COLL_LENGTH_DECL, 0);
    g_keyed_id = idl_method_id_step(ctx, KEYED_ARGS, 4, NULL, 0, &COLL_KEYED_DECL, 0);
    g_keys_id = idl_method_id_step(ctx, KEYED_ARGS, 4, NULL, 0, &COLL_KEYS_DECL, 0);
    g_ready = 1;
    realm_declare_intrinsic(collections_install_protos);
}

/* §4.2.10's TWO INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. */
void collections_install_protos(JSContext *ctx)
{
    JSValue nlp, hcp, prev;

    DCHECK(g_ready, "a realm asked for a collection prototype before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_nodelist_class);
    DCHECK(JS_IsNull(prev), "collections_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    nlp = JS_NewObject(ctx);
    hcp = JS_NewObject(ctx);
    CHECK(!JS_IsException(nlp) && !JS_IsException(hcp), "a collection prototype could not be allocated");
    idl_interface_tag(ctx, nlp, "NodeList");
    idl_interface_tag(ctx, hcp, "HTMLCollection");
    idl_install_accessor_step(ctx, nlp, "length", g_length_id, -1);
    idl_install_accessor_step(ctx, hcp, "length", g_length_id, -1);
    idl_install_method(ctx, nlp, "item", g_item_id);
    idl_install_method(ctx, hcp, "item", g_item_id);
    idl_install_method(ctx, hcp, "namedItem", g_named_item_id);
    /* Web IDL §3.7.9 Iterable declarations step 1.2: NodeList declares `iterable<Node>`; HTMLCollection none. */
    idl_indexed_install_value_iterator(ctx, nlp);
    JS_SetClassProto(ctx, g_nodelist_class, nlp);
    JS_SetClassProto(ctx, g_htmlcoll_class, hcp);
}

JSValue nodelist_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_nodelist_class);
    DCHECK(!JS_IsNull(proto), "NodeList.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

JSValue htmlcollection_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_htmlcoll_class);
    DCHECK(!JS_IsNull(proto), "HTMLCollection.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

/* An HTMLCollection, in THIS realm — the three call sites that build one differ only in their query. */
static JSValue coll_new_hc(JSContext *ctx, int kind, JSValueConst owner, const char *name, const char *ns)
{
    JSValue proto = htmlcollection_proto(ctx);
    JSValue r = coll_new(ctx, proto, &HTMLCOLL_INDEXED, kind, owner, name, ns);
    JS_FreeValue(ctx, proto);
    return r;
}

void collections_install(JSContext *ctx, JSValueConst global)
{
    JSValue nl, hc;

    DCHECK(g_ready, "the collection interfaces were installed before their prototypes were built");
    {
        JSValue nlp = nodelist_proto(ctx), hcp = htmlcollection_proto(ctx);
        nl = idl_interface_object(ctx, "NodeList", nlp);
        hc = idl_interface_object(ctx, "HTMLCollection", hcp);
        JS_FreeValue(ctx, nlp);
        JS_FreeValue(ctx, hcp);
    }
    idl_define_global_property_reference(ctx, global, "NodeList", nl);
    idl_define_global_property_reference(ctx, global, "HTMLCollection", hc);
}

void collections_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — released with their contexts */
    JS_FreeValueRT(rt, g_cache_key);
    g_key = g_cache_key = JS_UNDEFINED;
    g_ready = 0;
    /* THE TWO CLASS IDS ARE NOT PUT BACK HERE — they are declared under `element`, whose release ends in
       agent_state_undo, and this states that the cascade reached this file so that line may put them back.
       See core/agent_state.h's agent_state_undo and agent_state_reached. */
    agent_state_reached("element");
}
