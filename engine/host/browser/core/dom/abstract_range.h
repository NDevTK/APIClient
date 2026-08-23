/* DOM §5.2 BOUNDARY POINTS, §5.3 AbstractRange AND §5.4 StaticRange.
 *
 * WHAT IS SHARED IS A PAIR OF BOUNDARY POINTS AND ONE COMPARISON, and both live here because §5 states them
 * once. A range — live or static — IS "two boundary points", §5.3's five getters read exactly those, and every
 * one of §5.5's own algorithms is phrased in terms of §5.2's "position of (nodeA, offsetA) relative to (nodeB,
 * offsetB)". Written twice, a Range and a StaticRange would answer `collapsed` by two rules.
 *
 * NOTHING IN §5.2, §5.3 OR §5.4 CAN RUN THE PAGE'S CODE once its Web IDL arguments are converted — the
 * comparison is a tree walk and the getters read one slot — so these are ordinary C. §5.5's members that walk a
 * subtree are §5.5's problem and live in range.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ABSTRACT_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ABSTRACT_RANGE_H

#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "solver/cow.h"

/* §5.3: "A range has two associated boundary points — a start and an end", and a boundary point is "a node and
   an offset". This IS the whole state of both §5.4's StaticRange and §5.5's Range, which is why one struct and
   one COW record serve both. */
typedef struct RangeBounds {
    JSValue  start_node;   /* the start boundary point's node (a wrapper; OWNED) */
    JSValue  end_node;     /* the end boundary point's node (a wrapper; OWNED) */
    uint32_t start_off, end_off;
} RangeBounds;

/* The per-flow COW capture list for a record that IS a RangeBounds — the same list the finalizer frees. */
extern const CowRecord RANGE_BOUNDS_REC;

void range_bounds_release(JSRuntime *rt, RangeBounds *b);
void range_bounds_mark(JSRuntime *rt, RangeBounds *b, JS_MarkFunc *mark_func);

/* §5.2: the POSITION of (a, ao) relative to (b, bo) — −1 before, 0 equal, +1 after. The two nodes must have the
   same root, which is the standard's own assertion and this engine's DCHECK. */
#define BP_BEFORE (-1)
#define BP_EQUAL    0
#define BP_AFTER    1
int boundary_position(lxb_dom_node_t *a, uint32_t ao, lxb_dom_node_t *b, uint32_t bo);

/* A §5 INTERFACE REGISTERS ITS INSTANCES' CLASS, so §5.3's getters — which are declared on AbstractRange and
   therefore serve both interfaces — can find the bounds on either. Two of them exist and the platform has no
   third, which the registration asserts. */
void abstract_range_claim_class(JSClassID cls);
/* The bounds behind any §5 range object, or NULL. Captures the record into the running flow's delta, which is
   what makes one flow's `setStart` invisible to a sibling. */
RangeBounds *abstract_range_of(JSValueConst v);

/* §5.4's "a new StaticRange" REACHED FROM C, with its four boundary-point components already decided. §5.4's
   own constructor is `new StaticRange(init)` and runs a dictionary conversion before it gets here; an
   algorithm of another standard that says "a new StaticRange whose start node is …" — Selection API §3's
   `getComposedRanges()` step 6 is the first — has no dictionary and must not build one to be read back.
   STEP 1's InvalidNodeTypeError is NOT re-asked: it is a check on the ARGUMENTS the page supplied, and a
   caller reaching here states boundary points the tree gave it, which this asserts instead. The two nodes are
   WRAPPERS and are BORROWED. OWNED. */
JSValue static_range_new(JSContext *ctx, JSValueConst snode, uint32_t soff, JSValueConst enode, uint32_t eoff);

void abstract_range_init(JSContext *ctx);
/* §5.3's and §5.4's INTERFACE PROTOTYPE OBJECTS for one realm — declared into core/realm.h's list. */
void abstract_range_install_protos(JSContext *ctx);
/* AbstractRange.prototype — the base §5.5's Range.prototype chains to. OWNED. */
JSValue abstract_range_proto(JSContext *ctx);
void abstract_range_install(JSContext *ctx, JSValueConst global);
void abstract_range_free(JSRuntime *rt);

#endif
