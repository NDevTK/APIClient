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

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"
#include "core/dom/node.h"
#include "core/dom/collections.h"
#include "solver/dom_cow.h"

static JSValue g_nodelist_proto = JS_UNDEFINED;
static JSValue g_htmlcoll_proto = JS_UNDEFINED;
static JSValue g_key = JS_UNDEFINED;        /* the collection's own slots, under a private Symbol */
static JSValue g_cache_key = JS_UNDEFINED;  /* the [SameObject] cache on an owner's wrapper */
static int     g_ready;

/* WHAT A COLLECTION IS OVER. The live kinds hold the owner and walk it per read; the static one holds an array
   and never looks at the tree again. */
enum { COLL_CHILD_NODES = 0, COLL_CHILDREN, COLL_STATIC };

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

/* Is this child one the collection counts? A NodeList counts every node; an HTMLCollection counts elements. */
static bool coll_takes(int kind, const lxb_dom_node_t *c)
{
    return kind == COLL_CHILD_NODES || c->type == LXB_DOM_NODE_TYPE_ELEMENT;
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

/* The cache for this collection, already checked against the tree it describes. NULL means walk from scratch. */
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
    for (c = n->first_child; c; c = c->next)
        if (coll_takes(kind, c)) count++;
    if (cc) { cc->length = count; cc->has_length = 1; }
    return count;
}

/* Step `from` by `delta` members of the collection, in `dir`. The members are the children `coll_takes` accepts,
   so a NodeList steps one child at a time and an HTMLCollection skips the text between elements. */
static lxb_dom_node_t *coll_step(int kind, lxb_dom_node_t *from, uint32_t delta, int forward)
{
    lxb_dom_node_t *c = from;
    while (delta) {
        c = forward ? c->next : c->prev;
        if (!c) return NULL;
        if (coll_takes(kind, c)) delta--;
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
    if (cc && cc->node) {
        /* THE WHOLE POINT: a loop that reads 0, 1, 2… moves one member per call rather than i of them. A read
           that goes BACKWARDS is the same walk through `prev`, because a child list is doubly linked and a page
           iterating in reverse deserves the same answer as one iterating forwards. */
        DCHECK(cc->node->parent == n,
               "a cached collection member is no longer a child of the collection's owner — a tree mutation "
               "did not advance dom_cow_version, so the cache is describing a tree that is gone");
        c = i >= cc->index ? coll_step(kind, cc->node, i - cc->index, 1)
                           : coll_step(kind, cc->node, cc->index - i, 0);
        if (!c) return JS_UNDEFINED;
        cc->index = i;
        cc->node = c;
        return node_wrap(ctx, c);
    }
    for (c = n->first_child; c; c = c->next) {
        if (!coll_takes(kind, c)) continue;
        if (k == i) {
            if (cc) { cc->index = i; cc->node = c; }
            return node_wrap(ctx, c);
        }
        k++;
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
    if (kind != COLL_CHILDREN || !n) return JS_UNDEFINED;
    for (c = n->first_child; c; c = c->next) {
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
                        JSValueConst owner)
{
    JSValue obj = idl_indexed_new(ctx, proto, decl), slots;
    JSAtom k;

    if (JS_IsException(obj)) return obj;
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "collections: OOM allocating a collection's slots");
    JS_SetPropertyStr(ctx, slots, "kind", JS_NewInt32(ctx, kind));
    JS_SetPropertyStr(ctx, slots, "owner", JS_DupValue(ctx, owner));
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
        coll = coll_new(ctx, proto, decl, kind, owner);
        JS_SetPropertyUint32(ctx, cache, (uint32_t)kind, JS_DupValue(ctx, coll));
    }
    JS_FreeValue(ctx, cache);
    return coll;
}

JSValue collections_child_nodes(JSContext *ctx, JSValueConst owner)
{
    return coll_cached(ctx, owner, COLL_CHILD_NODES, g_nodelist_proto, &NODELIST_INDEXED);
}

JSValue collections_children(JSContext *ctx, JSValueConst owner)
{
    return coll_cached(ctx, owner, COLL_CHILDREN, g_htmlcoll_proto, &HTMLCOLL_INDEXED);
}

JSValue collections_static(JSContext *ctx, JSValue nodes)
{
    JSValue coll;

    DCHECK(g_ready, "a static NodeList was built before collections_init ran");
    coll = coll_new(ctx, g_nodelist_proto, &NODELIST_INDEXED, COLL_STATIC, nodes);
    JS_FreeValue(ctx, nodes);   /* the slots hold their own reference */
    return coll;
}

void collections_init(JSContext *ctx)
{
    DCHECK(!g_ready, "collections_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "collectionSlots", false);
    g_cache_key = JS_NewSymbol(ctx, "collectionCache", false);
    CHECK(!JS_IsException(g_key) && !JS_IsException(g_cache_key),
          "the collection slot keys could not be allocated");
    g_nodelist_proto = JS_NewObject(ctx);
    g_htmlcoll_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_nodelist_proto) && !JS_IsException(g_htmlcoll_proto),
          "a collection prototype could not be allocated");
    {
        static const IdlArgType ONE_LONG[1] = { IDL_LONG };
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        int item_id = idl_method_id(ctx, ONE_LONG, 1, js_coll_item, 0);
        idl_install_accessor(ctx, g_nodelist_proto, "length", js_coll_length, 0, -1);
        idl_install_accessor(ctx, g_htmlcoll_proto, "length", js_coll_length, 0, -1);
        idl_install_method(ctx, g_nodelist_proto, "item", 1, item_id);
        idl_install_method(ctx, g_htmlcoll_proto, "item", 1, item_id);
        idl_install_method(ctx, g_htmlcoll_proto, "namedItem", 1,
                           idl_method_id(ctx, ONE_STR, 1, js_coll_named_item, 0));
    }
    /* §3.7.10: NodeList's IDL declares `iterable<Node>`, so it gets the value-iterator members. HTMLCollection
       declares NO iterable — it is iterable only through the indexed getter, which §3.7.10 gives @@iterator
       for and nothing else. Two interfaces, two answers, because that is what the two IDLs say. */
    idl_indexed_install_iterable(ctx, g_nodelist_proto);
    idl_indexed_install_iterable(ctx, g_htmlcoll_proto);
    g_ready = 1;
}

void collections_install(JSContext *ctx, JSValueConst global)
{
    JSValue nl, hc;

    DCHECK(g_ready, "the collection interfaces were installed before their prototypes were built");
    nl = JS_NewCFunction2(ctx, NULL, "NodeList", 0, JS_CFUNC_constructor, 0);
    hc = JS_NewCFunction2(ctx, NULL, "HTMLCollection", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(nl) && !JS_IsException(hc),
          "a collection interface object could not be allocated");
    JS_SetConstructor(ctx, nl, g_nodelist_proto);
    JS_SetConstructor(ctx, hc, g_htmlcoll_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "NodeList", nl);
    JS_SetPropertyStr(ctx, (JSValue)global, "HTMLCollection", hc);
}

void collections_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_nodelist_proto);
    JS_FreeValue(ctx, g_htmlcoll_proto);
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_cache_key);
    g_nodelist_proto = g_htmlcoll_proto = g_key = g_cache_key = JS_UNDEFINED;
    g_ready = 0;
}
