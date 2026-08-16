/* CSS Cascade §6.4.3 — the LAYER ORDER: the single sequence every cascade layer in one origin occupies, which
 * §6.1's Layers criterion sorts declarations by.
 *
 * IT IS A TREE THAT FLATTENS, NOT A LIST THAT APPENDS, and §6.4.3 says so in one sentence: "Cascade layers are
 * sorted by the order in which they first are declared, WITH NESTED LAYERS GROUPED WITHIN THEIR PARENT LAYER."
 * A flat first-seen list would put `framework.theme` wherever it was first written; the standard puts it inside
 * `framework`, between `framework`'s earlier children and `framework`'s own rules — so the structure has to be
 * the nesting and the order has to be a walk of it. §6.4.3's worked example is the whole specification of that
 * walk, and it is the fixture this component is exercised with:
 *     h1 { }  @layer reset.type { }  @layer framework { .title { } @layer theme { } }  @layer reset { }
 * orders as `reset.type`, `reset` (implicit sub-layer), `framework.theme`, `framework` (implicit sub-layer),
 * (implicit outer layer) — which is a POST-ORDER walk: each node's explicit children in first-declaration order,
 * then the node itself. The node ITSELF is its implicit sub-layer, because §6.4.3's second sentence says where a
 * layer's own directly-held rules go: "style rules without further nesting are similarly added to an implicit
 * sub-layer AFTER the explicitly nested layers."
 *
 * UNLAYERED IS LAST, NOT FIRST, AND LAST IS THE STRONG END FOR NORMAL DECLARATIONS. §6.4.3: "Unlayered rules
 * are sorted LATER than any layered rules within the same parent layer (if any)", and §6.1: "for normal rules
 * the declaration whose cascade layer is LATEST in the layer order wins". So the ROOT of this tree is the
 * implicit outer layer, it takes the LAST index, and a page's ordinary unlayered rules beat everything any
 * `@layer` block holds — §6.4's own opening example turns on exactly that ("The unlayered declarations on the
 * audio element take precedence over the explicitly layered declarations on audio[controls]—even though the
 * unlayered styles have a lower specificity, and come first in the order of appearance"). Reading the order the
 * other way round is the mistake this paragraph exists to prevent: it inverts every page that uses `@layer` at
 * all, and it inverts it INVISIBLY, because both readings produce a real value.
 *
 * AN ANONYMOUS SEGMENT IS A FRESH NODE EVERY TIME, which is §6.4.2.1 verbatim: "Each occurrence of an anonymous
 * layer declaration represents a unique cascade layer", so `@layer { }` twice is two layers while
 * `@layer a { }` twice is one. That is why `declare` takes a PARENT NODE rather than a dotted path from the
 * root — a `@layer foo` inside one anonymous block and a `@layer foo` inside another are different layers, and
 * only the parent node can say so ("in separate unnamed layers, child layers with the same name refer to
 * different cascade layers, because they have distinct anonymous parent layers").
 *
 * A NAME IS COMPARED BY ITS SEGMENTS, and the segments arrive SERIALIZED. §6.4.2: "Layer names represent the
 * same cascade layer if they contain the same segments in the same order"; core/css/css_at_rule_prelude.h puts
 * every segment through CSSOM §2.1's serialize-an-identifier and joins with `.`, which is what makes a byte
 * comparison the identity test — the escapes are normalised and the case is the author's, which is correct
 * because a CSS identifier is case-SENSITIVE. The split back into segments is that same component's, for the
 * reason it owns the join: `a\.b` is ONE segment containing a period and `a.b` is two, and one file has to hold
 * that convention.
 *
 * IT IS SEALED BEFORE IT IS READ, because an index is a fact about the WHOLE order and not about the node. A
 * layer declared after this element's sheets were walked would renumber everything, so the read asserts the
 * seal rather than computing an index that a later declaration could invalidate — which is the difference
 * between an order and a counter.
 *
 * IT IS REBUILT PER CASCADE, exactly as the sheet text it is walked out of is (core/css/css_style_declaration.c
 * says why: a cache here would be shared state the flow machinery does not swap, so a layer one arm declared
 * would order another arm's cascade). The order is GLOBAL TO THE DOCUMENT — §6.4.3's note says so — which here
 * means global to the sheet list one resolution reads, walked in document order so first-declaration order is
 * the walk's own order. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_LAYER_ORDER_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_LAYER_ORDER_H

typedef struct CssLayerOrder CssLayerOrder;
typedef struct CssLayerNode  CssLayerNode;

/* An order holding nothing but §6.4.3's implicit outer layer. */
CssLayerOrder *css_layer_order_create(void);
void css_layer_order_free(CssLayerOrder *o);

/* §6.4.3's IMPLICIT OUTER LAYER — the layer an unlayered rule is in, and the parent every top-level `@layer`
   declares under. It is the node with the LAST index once the order is sealed. */
CssLayerNode *css_layer_order_root(CssLayerOrder *o);

/* DECLARE a layer, returning the node its rules belong to. `name` is one serialized `<layer-name>` — a dotted
   one is §6.4.2's "shorthand representing those layers nested in order" and creates a node per segment — or
   NULL for §6.4.2.1's anonymous layer, which mints a node no later declaration can name. Declaring a name that
   `parent` already holds ANSWERS THE EXISTING NODE and does not move it: §6.4.3 orders by the order in which
   layers FIRST are declared. */
CssLayerNode *css_layer_order_declare(CssLayerOrder *o, CssLayerNode *parent, const char *name);

/* Flatten §6.4.3's tree into the order, assigning every node its index. No declaration may follow. */
void css_layer_order_seal(CssLayerOrder *o);

/* How many layers the order holds — always at least one, the implicit outer layer. Sealed only. */
unsigned css_layer_order_count(const CssLayerOrder *o);

/* A node's position in §6.4.3's order: 0 is the earliest layer and `count - 1` is the implicit outer layer.
   Sealed only. §6.1 reads it in BOTH directions — latest wins for a normal declaration, earliest for an
   important one — which is core/css/css_cascade.h's arithmetic and not this component's. */
unsigned css_layer_order_index(const CssLayerOrder *o, const CssLayerNode *n);

#endif
