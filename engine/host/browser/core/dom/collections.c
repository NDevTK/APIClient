/* NodeList and HTMLCollection — DOM §4.2.10 and §4.2.11.
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
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_indexed.h"
#include "core/dom/node.h"
#include "core/dom/collections.h"
#include "solver/dom_cow.h"

/* PER REALM — §3.7, and here it decides ANSWERS: a C member runs in the realm that DEFINED it, so one shared
   prototype answers every document out of whichever realm built it first. Held in quickjs's per-context
   class-proto slots. */
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
       COLL_LINKS, COLL_NAMED };

static bool coll_is_descendant(int kind)
{
    return kind == COLL_BY_TAG || kind == COLL_BY_TAG_NS || kind == COLL_BY_CLASS ||
           kind == COLL_LINKS || kind == COLL_NAMED;
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

/* §4.9's class matching: the element must carry EVERY token the query asks for. Both sides are ASCII-whitespace
   separated token lists, which is why this is a nested scan and not a string compare — `getElementsByClassName
   ('a b')` matches `class="b x a"`. */
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
    /* §4.9: an EMPTY token list matches nothing, which is not the same as matching everything. */
    return any;
}

/* ASCII case-insensitive compare — §4.5's rule is ASCII-only, so a locale-aware one would be wrong. */
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
/* §4.5 STATES TWO BY-NAME ALGORITHMS AND THEY TAKE DIFFERENT OPERANDS. "list of elements with qualified name
   qualifiedName" matches ONE string against the qualified name, with the HTML-document lowercasing rule.
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

/* Does this element's namespace match the query's? §4.5: the empty string was already turned into null by
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

static bool coll_takes(int kind, const CollQuery *qy, const lxb_dom_node_t *c)
{
    const char *name = qy ? qy->name : NULL;
    size_t nlen = qy ? qy->nlen : 0;

    if (kind == COLL_CHILD_NODES) return true;
    if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (kind == COLL_BY_TAG) {
        size_t qn = 0;
        const lxb_char_t *q;
        /* §4.5: `*` is every element, and it is the form a page uses to count a subtree. */
        if (nlen == 1 && name[0] == '*') return true;
        q = lxb_dom_element_qualified_name((lxb_dom_element_t *)c, &qn);
        if (!q || qn != nlen) return false;
        /* §4.5: in an HTML document the query is matched against the qualified name LOWERCASED, for elements in
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
    if (kind == COLL_NAMED) {
        /* HTML §7.3.3's NAMED ELEMENTS, which is two rules and not one: any HTML element whose `id` is the
           name, and `embed`/`form`/`img`/`object` whose `name` attribute is. The tag restriction is on the
           `name` half only — a `<div name=x>` is not a named element, a `<div id=x>` is. */
        lxb_dom_element_t *el = (lxb_dom_element_t *)c;
        size_t vl = 0, qn = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"id", 2, &vl);
        const lxb_char_t *q;
        if (v && vl == nlen && memcmp(v, name, nlen) == 0) return true;
        q = lxb_dom_element_qualified_name(el, &qn);
        if (!q) return false;
        if (!((qn == 5 && memcmp(q, "embed", 5) == 0) || (qn == 4 && memcmp(q, "form", 4) == 0) ||
              (qn == 3 && memcmp(q, "img", 3) == 0)   || (qn == 6 && memcmp(q, "object", 6) == 0) ||
              (qn == 6 && memcmp(q, "iframe", 6) == 0)))
            return false;
        vl = 0;
        v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"name", 4, &vl);
        return v && vl == nlen && memcmp(v, name, nlen) == 0;
    }
    if (kind == COLL_LINKS) {
        /* §3.1.5 `document.links` is `a` AND `area` elements THAT HAVE AN href — the attribute is half the
           definition, so an anchor used as a scroll target is not a link. */
        size_t qn = 0, vl = 0;
        const lxb_char_t *q = lxb_dom_element_qualified_name((lxb_dom_element_t *)c, &qn);
        if (!q || !((qn == 1 && q[0] == 'a') || (qn == 4 && memcmp(q, "area", 4) == 0))) return false;
        return lxb_dom_element_get_attribute((lxb_dom_element_t *)c,
                                             (const lxb_char_t *)"href", 4, &vl) != NULL;
    }
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

/* THE INDEX CACHE — Blink's CollectionIndexCache, and the reason it has to exist here too.
   A live collection has nothing to cache the tree IN, so counting it is a walk and reaching index `i` is a walk
   of `i`. That makes the loop every page writes — `for (i = 0; i < el.children.length; i++) el.children[i]` —
   quadratic in the page's own markup, and it runs inside an exotic [[GetOwnProperty]], which is a synchronous C
   contract with nowhere to suspend. Being O(1) is what makes a body safe to leave un-parkable, so this is what
   makes the indexed getter safe: not a way to yield out of the walk, but no walk to yield out of.
   The cache is a MEMO, valid exactly while the tree version is unchanged — which the swap advances, so a cache
   another flow filled is already invalid by the time this one runs. Nothing here is per-flow state; a miss
   costs the walk that used to happen every time. */
typedef struct {
    uint64_t        version;      /* the tree this describes; never 0, because the version starts at 1 */
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

/* The cache for this collection, already checked against the tree it describes. NULL means walk from scratch. */
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
           string, which §4.5's first step has already turned into null and which would otherwise match an
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

static CollCache *coll_cache(JSValueConst self, lxb_dom_node_t *owner)
{
    CollCache *c = idl_indexed_cache(self);
    if (!c) return NULL;
    if (c->version != dom_cow_version() || c->owner != owner) {
        memset(c, 0, sizeof *c);
        c->version = dom_cow_version();
        c->owner = owner;
    }
    return c;
}

static uint32_t coll_length(JSContext *ctx, JSValueConst self)
{
    JSValue owner;
    int kind = coll_kind(ctx, self, &owner);
    lxb_dom_node_t *n, *c;
    uint32_t count = 0;
    CollCache *cc;

    if (kind == COLL_STATIC) {
        JSValue lv = JS_GetPropertyStr(ctx, owner, "length");
        JS_ToUint32(ctx, &count, lv);
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, owner);
        return count;
    }
    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    if (!n) return 0;
    cc = coll_cache(self, n);
    if (cc && cc->has_length) return cc->length;
    /* THE TREE AS IT IS NOW. That walk is the liveness — there is nothing here to invalidate or refresh,
       because the version says when what was counted stopped describing the tree. */
    {
        CollQuery q;
        coll_query(ctx, self, kind, &q);
        for (c = coll_first_node(kind, n); c; c = coll_adv(kind, n, c, 1))
            if (coll_takes(kind, &q, c)) count++;
        coll_query_free(ctx, &q);
    }
    if (cc) { cc->length = count; cc->has_length = 1; }
    return count;
}

/* Step `from` by `delta` members of the collection, in `dir`. The members are the children `coll_takes` accepts,
   so a NodeList steps one child at a time and an HTMLCollection skips the text between elements. */
static lxb_dom_node_t *coll_step(int kind, const CollQuery *q, lxb_dom_node_t *root,
                                 lxb_dom_node_t *from, uint32_t delta, int forward)
{
    lxb_dom_node_t *c = from;
    while (delta) {
        c = coll_adv(kind, root, c, forward);
        if (!c) return NULL;
        if (coll_takes(kind, q, c)) delta--;
    }
    return c;
}

static JSValue coll_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue owner;
    int kind = coll_kind(ctx, self, &owner);
    lxb_dom_node_t *n, *c;
    uint32_t k = 0;
    JSValue r = JS_UNDEFINED;
    CollCache *cc;

    if (kind == COLL_STATIC) {
        r = JS_GetPropertyUint32(ctx, owner, i);
        JS_FreeValue(ctx, owner);
        return r;
    }
    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    if (!n) return JS_UNDEFINED;
    cc = coll_cache(self, n);
    {
    CollQuery q;
    coll_query(ctx, self, kind, &q);
    if (cc && cc->node) {
        /* THE WHOLE POINT: a loop that reads 0, 1, 2… moves one member per call rather than i of them. A read
           that goes BACKWARDS is the same walk through `prev`, because a child list is doubly linked and a page
           iterating in reverse deserves the same answer as one iterating forwards. */
        /* THE CACHED MEMBER IS STILL IN THE COLLECTION'S SUBTREE. For the child kinds that is one pointer; for
           a descendant kind it is a walk up to the owner, which is O(depth) and dev-only. Writing it as
           `parent == n` was right for the two kinds that existed and silently wrong for the two that did not —
           a descendant is not a child, and the assert fired on the first by-name collection built. */
        DCHECK(coll_in_subtree(cc->node, n),
               "a cached collection member has left the collection's owner — a tree mutation did not advance "
               "dom_cow_version, so the cache is describing a tree that is gone");
        c = i >= cc->index ? coll_step(kind, &q, n, cc->node, i - cc->index, 1)
                           : coll_step(kind, &q, n, cc->node, cc->index - i, 0);
        coll_query_free(ctx, &q);
        if (!c) return JS_UNDEFINED;
        cc->index = i;
        cc->node = c;
        return node_wrap(ctx, c);
    }
    for (c = coll_first_node(kind, n); c; c = coll_adv(kind, n, c, 1)) {
        if (!coll_takes(kind, &q, c)) continue;
        if (k == i) {
            if (cc) { cc->index = i; cc->node = c; }
            coll_query_free(ctx, &q);
            return node_wrap(ctx, c);
        }
        k++;
    }
    coll_query_free(ctx, &q);
    }
    return JS_UNDEFINED;
}

/* §4.2.11's NAMED getter: the first element whose `id` is `name`, or — for the elements whose content
   attribute the spec lists — whose `name` is. Only HTMLCollection has one; a NodeList's IDL declares none, so
   `nodes.foo` is honestly undefined rather than a search that finds nothing. */
static JSValue coll_named(JSContext *ctx, JSValueConst self, const char *name)
{
    JSValue owner;
    int kind = coll_kind(ctx, self, &owner);
    lxb_dom_node_t *n, *c;
    size_t nlen = strlen(name), vlen = 0;

    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    /* §4.2.11's named getter belongs to HTMLCollection, which is every kind here except the two NodeLists. */
    if (!n || (kind != COLL_CHILDREN && !coll_is_descendant(kind))) return JS_UNDEFINED;
    for (c = coll_first_node(kind, n); c; c = coll_adv(kind, n, c, 1)) {
        const lxb_char_t *v;
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        v = lxb_dom_element_get_attribute(lxb_dom_interface_element(c), (const lxb_char_t *)"id", 2, &vlen);
        if (v && vlen == nlen && memcmp(v, name, nlen) == 0) return node_wrap(ctx, c);
        v = lxb_dom_element_get_attribute(lxb_dom_interface_element(c), (const lxb_char_t *)"name", 4, &vlen);
        if (v && vlen == nlen && memcmp(v, name, nlen) == 0) return node_wrap(ctx, c);
    }
    return JS_UNDEFINED;
}

/* A LIVE collection carries the index cache; the STATIC one does not, because its item() is already an array
   read and a cache would be a second copy of the answer to keep in step. Both kinds share one decl per
   interface, so the live decl's cache_size is what a static NodeList allocates too — a few bytes it never
   reads, against a second decl that differs in one field. */
static const IdlIndexedDecl NODELIST_INDEXED = { "NodeList", coll_length, coll_item, NULL, sizeof(CollCache) };
static const IdlIndexedDecl HTMLCOLL_INDEXED = { "HTMLCollection", coll_length, coll_item, coll_named,
                                                 sizeof(CollCache) };

/* ---- the members ------------------------------------------------------------------------------------------ */
static JSValue js_coll_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return JS_NewUint32(ctx, coll_length(ctx, this_val));
}

/* §4.2.10 item(index) — null past the end, which is what makes it different from the indexed getter. */
static JSValue js_coll_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    int64_t i = 0;
    JSValue r;

    (void)magic;
    if (argc < 1) return JS_NULL;
    JS_ToInt64(ctx, &i, argv[0]);   /* a real number by now: the declaration converted it */
    if (i < 0) return JS_NULL;
    r = coll_item(ctx, this_val, (uint32_t)i);
    return JS_IsUndefined(r) ? JS_NULL : r;
}

static JSValue js_coll_named_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *name;
    JSValue r;

    (void)magic;
    if (argc < 1) return JS_NULL;
    name = JS_ToCString(ctx, argv[0]);   /* a real string by now */
    if (!name) return JS_EXCEPTION;
    r = coll_named(ctx, this_val, name);
    JS_FreeCString(ctx, name);
    return JS_IsUndefined(r) ? JS_NULL : r;
}

/* ---- construction ------------------------------------------------------------------------------------------ */
static JSValue coll_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl, int kind,
                        JSValueConst owner, const char *name, const char *ns)
{
    JSValue obj = idl_indexed_new(ctx, proto, decl), slots;
    JSAtom k;

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

/* §4.5/§4.9 getElementsByTagName / getElementsByClassName — LIVE, and not [SameObject]: the spec returns a new
   collection per call (unlike childNodes), because the query is part of what the collection IS. */
JSValue collections_by_name(JSContext *ctx, JSValueConst owner, const char *name, bool by_class)
{
    DCHECK(g_ready, "a by-name collection was built before collections_init ran");
    return coll_new_hc(ctx, by_class ? COLL_BY_CLASS : COLL_BY_TAG, owner, name, NULL);
}

/* §4.5's "list of elements with namespace namespace and local name localName" — getElementsByTagNameNS on both
   Document and Element. LIVE and not [SameObject], like its qualified-name sibling. `ns` NULL is the NULL
   NAMESPACE, which matches an element in no namespace; the empty string became null at the member. */
JSValue collections_by_tag_ns(JSContext *ctx, JSValueConst owner, const char *ns, const char *local)
{
    DCHECK(g_ready, "a by-namespace collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_BY_TAG_NS, owner, local, ns);
}

/* §3.1.5's `document.links` — `a`/`area` WITH an href, which is a predicate rather than a name, so it is its
   own kind rather than a by-tag collection that would also count the anchors with no href. */
JSValue collections_named(JSContext *ctx, JSValueConst owner, const char *name)
{
    DCHECK(g_ready, "a named collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_NAMED, owner, name, NULL);
}

JSValue collections_links(JSContext *ctx, JSValueConst owner)
{
    DCHECK(g_ready, "a links collection was built before collections_init ran");
    return coll_new_hc(ctx, COLL_LINKS, owner, NULL, NULL);
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
    static const IdlArgType ONE_LONG[1] = { IDL_LONG };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
    JSClassDef nl = { "NodeList" }, hc = { "HTMLCollection" };

    DCHECK(!g_ready, "collections_init ran twice — the interfaces are declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "collectionSlots", false);
    g_cache_key = JS_NewSymbol(ctx, "collectionCache", false);
    CHECK(!JS_IsException(g_key) && !JS_IsException(g_cache_key),
          "the collection slot keys could not be allocated");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nodelist_class);
    JS_NewClass(JS_GetRuntime(ctx), g_nodelist_class, &nl);
    JS_NewClassID(JS_GetRuntime(ctx), &g_htmlcoll_class);
    JS_NewClass(JS_GetRuntime(ctx), g_htmlcoll_class, &hc);
    /* ONE declaration for `item`, installed on BOTH prototypes — it is the same operation with the same
       conversion in both IDLs, and two pool entries would be two copies of one fact. */
    g_item_id = idl_method_id(ctx, ONE_LONG, 1, js_coll_item, 0);
    g_named_item_id = idl_method_id(ctx, ONE_STR, 1, js_coll_named_item, 0);
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
    idl_install_accessor_no_user_code(ctx, nlp, "length", js_coll_length, 0, -1);
    idl_install_accessor_no_user_code(ctx, hcp, "length", js_coll_length, 0, -1);
    idl_install_method(ctx, nlp, "item", g_item_id);
    idl_install_method(ctx, hcp, "item", g_item_id);
    idl_install_method(ctx, hcp, "namedItem", g_named_item_id);
    /* §3.7.10: NodeList's IDL declares `iterable<Node>`, so it gets the value-iterator members. HTMLCollection
       declares NO iterable — it is iterable only through the indexed getter, which §3.7.10 gives @@iterator
       for and nothing else. Two interfaces, two answers, because that is what the two IDLs say. */
    idl_indexed_install_iterable(ctx, nlp);
    idl_indexed_install_value_iterator(ctx, nlp);   /* §4.2.10 `iterable<Node>` */
    idl_indexed_install_iterable(ctx, hcp);
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
    JS_SetPropertyStr(ctx, (JSValue)global, "NodeList", nl);
    JS_SetPropertyStr(ctx, (JSValue)global, "HTMLCollection", hc);
}

void collections_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — released with their contexts */
    JS_FreeValueRT(rt, g_cache_key);
    g_key = g_cache_key = JS_UNDEFINED;
    g_ready = 0;
}
