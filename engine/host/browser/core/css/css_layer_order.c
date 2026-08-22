/* CSS Cascade §6.4.3's layer order. See css_layer_order.h for why it is a tree, why the walk is post-order, why
   the implicit outer layer takes the LAST index, and why an anonymous segment is a fresh node every time. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_at_rule_prelude.h"
#include "core/css/css_layer_order.h"

struct CssLayerNode {
    /* ONE SEGMENT of §6.4.2's layer name, serialized — NULL for §6.4.2.1's anonymous segment, which is why the
       lookup below tests the pointer before it tests the bytes: an anonymous node matches no name at all, not
       even another anonymous one. OWNED. */
    char         *name;
    CssLayerNode *first, *last;   /* the explicit children, in FIRST-DECLARATION order */
    CssLayerNode *next;           /* the next sibling in that order */
    unsigned      index;          /* §6.4.3's position, written by the seal */
};

struct CssLayerOrder {
    CssLayerNode root;   /* §6.4.3's implicit outer layer, which every top-level layer is declared under */
    unsigned     n;      /* how many nodes the seal numbered */
    bool         sealed;
};

CssLayerOrder *css_layer_order_create(void)
{
    CssLayerOrder *o = calloc(1, sizeof(*o));

    CHECK(o != NULL, "cssom: OOM building CSS Cascade §6.4.3's layer order");
    return o;
}

static void layer_node_free_children(CssLayerNode *n)
{
    CssLayerNode *c = n->first, *next;

    while (c) {
        next = c->next;
        layer_node_free_children(c);
        free(c->name);
        free(c);
        c = next;
    }
}

void css_layer_order_free(CssLayerOrder *o)
{
    if (!o) return;
    layer_node_free_children(&o->root);
    DCHECK(o->root.name == NULL,
           "§6.4.3's implicit outer layer has a NAME — it is the layer an UNLAYERED rule is in, so there is no "
           "`<layer-name>` that could refer to it and nothing that could have written one");
    free(o);
}

CssLayerNode *css_layer_order_root(CssLayerOrder *o)
{
    DCHECK(o != NULL, "§6.4.3's implicit outer layer was asked for through no layer order");
    return &o->root;
}

/* The child of `parent` that §6.4.2 calls the same cascade layer as `name`, or NULL. An ANONYMOUS declaration
   never matches: §6.4.2 says "anonymous segments have unique identities for each occurrence", so it is the one
   lookup whose answer is always no. */
static CssLayerNode *layer_child_named(CssLayerNode *parent, const char *name)
{
    CssLayerNode *c;

    if (!name) return NULL;
    for (c = parent->first; c; c = c->next)
        if (c->name && strcmp(c->name, name) == 0) return c;
    return NULL;
}

/* Append a fresh node under `parent`. `name` is COPIED, or NULL for an anonymous segment. */
static CssLayerNode *layer_child_append(CssLayerNode *parent, const char *name)
{
    CssLayerNode *n = calloc(1, sizeof(*n));

    CHECK(n != NULL, "cssom: OOM declaring a CSS Cascade §6.4 cascade layer");
    if (name) {
        n->name = strdup(name);
        CHECK(n->name != NULL, "cssom: OOM copying a `<layer-name>` segment into §6.4.3's layer order");
    }
    if (parent->last) parent->last->next = n;
    else              parent->first = n;
    parent->last = n;
    return n;
}

CssLayerNode *css_layer_order_declare(CssLayerOrder *o, CssLayerNode *parent, const char *name)
{
    CssLayerNames segs = { NULL, 0 };
    CssLayerNode *at;
    unsigned i;

    DCHECK(o != NULL && parent != NULL, "a cascade layer was declared into no order, or under no parent layer");
    DCHECK(!o->sealed,
           "a cascade layer was declared into a SEALED §6.4.3 order. An index is a fact about the whole order — "
           "a layer declared after the numbering would renumber every layer after it — so the walk that "
           "declares layers and the pass that reads their indices are two phases and not one");
    /* §6.4.2.1's anonymous layer: "when a @layer rule omits its <layer-name> ... its layer name gains a unique
       anonymous segment; it therefore cannot be referenced from the outside." One segment, never matched. */
    if (!name) return layer_child_append(parent, NULL);
    css_layer_name_segments(name, &segs);
    DCHECK(segs.n >= 1,
           "a `<layer-name>` split into NO segments. The production's first term is an `<ident>` and "
           "core/css/css_at_rule_prelude.c refuses a name without one, so an empty name is a rule that should "
           "never have been built — §6.4.2.1's anonymous layer arrives as a NULL name and not as an empty one");
    at = parent;
    /* §6.4.2: "When multiple identifiers are concatenated with a period, this is a shorthand representing those
       layers nested in order" — so `reset.type` declares `reset` and then `type` inside it, and `reset` is
       thereby FIRST DECLARED here whether or not a `@layer reset` block ever appears. */
    for (i = 0; i < segs.n; i++) {
        CssLayerNode *found = layer_child_named(at, segs.v[i]);

        at = found ? found : layer_child_append(at, segs.v[i]);
    }
    css_layer_names_free(&segs);
    return at;
}

/* §6.4.3's flatten. POST-ORDER: "nested layers are sorted in appearance order, and style rules without further
   nesting are similarly added to an implicit sub-layer AFTER the explicitly nested layers" — so a node's
   children are numbered first, in declaration order, and the node's own implicit sub-layer takes the next
   index. The root is therefore numbered last, which is §6.4.3's "unlayered rules are sorted later than any
   layered rules within the same parent layer". */
static unsigned layer_seal_walk(CssLayerNode *n, unsigned next)
{
    CssLayerNode *c;

    for (c = n->first; c; c = c->next) next = layer_seal_walk(c, next);
    n->index = next;
    return next + 1;
}

void css_layer_order_seal(CssLayerOrder *o)
{
    DCHECK(o != NULL, "§6.4.3's layer order was sealed through no order");
    DCHECK(!o->sealed, "§6.4.3's layer order was sealed twice — the second numbering would be identical, which "
                       "is why this is an invariant and not a no-op: a caller sealing again is a caller that "
                       "has lost track of which phase it is in");
    o->n = layer_seal_walk(&o->root, 0);
    o->sealed = true;
    DCHECK(o->root.index == o->n - 1,
           "§6.4.3's IMPLICIT OUTER LAYER is not last in the layer order. It holds every unlayered rule, and "
           "§6.4.3 sorts unlayered rules later than any layered rule — so the post-order walk must number the "
           "root after every node under it, and a root that is not at `count - 1` means the walk emitted a "
           "node after it");
}

unsigned css_layer_order_count(const CssLayerOrder *o)
{
    DCHECK(o != NULL, "§6.4.3's layer count was asked for through no order");
    DCHECK(o->sealed, "§6.4.3's layer count was read before the order was sealed, so no layer has an index yet");
    DCHECK(o->n >= 1, "§6.4.3's layer order holds NO layers — the implicit outer layer is always one of them, "
                      "and it exists before any `@layer` rule is met");
    return o->n;
}

unsigned css_layer_order_index(const CssLayerOrder *o, const CssLayerNode *n)
{
    DCHECK(o != NULL && n != NULL, "a cascade layer's §6.4.3 index was read through no order, or off no layer");
    DCHECK(o->sealed,
           "a cascade layer's §6.4.3 index was read before the order was sealed. Until the whole sheet list has "
           "been walked the index is not yet a fact — a layer first declared by a later sheet is ordered ahead "
           "of one declared after it — so a read here is a cascade that started sorting before it finished "
           "collecting");
    DCHECK(n->index < o->n, "a cascade layer's §6.4.3 index is past the end of the order it was numbered in — "
                            "the node belongs to a different layer order than the one it was read through");
    return n->index;
}
