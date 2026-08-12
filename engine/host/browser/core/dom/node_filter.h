/* NODEFILTER — DOM §6.3, and §6.4's "filter a node", which is the one algorithm NodeIterator and TreeWalker
 * share.
 *
 * WHY THE SHARED PART IS ITS OWN COMPONENT. §6 states the state ONCE — "Each NodeIterator and TreeWalker object
 * has an associated is active ..., root ..., whatToShow ..., and filter" — and then states ONE algorithm over
 * it. Written twice, the two interfaces would each carry their own bit test, their own callable-vs-object rule
 * and their own re-entrancy guard, and the first divergence between them would be invisible: a TreeWalker whose
 * `whatToShow` test is off by one still walks, it just walks a different tree than the standard's.
 *
 * AND THE FILTER RUNS THE PAGE'S CODE IN THE MIDDLE OF A TREE WALK, which is what makes every traversal member a
 * step machine rather than a C loop. §6.4 step 6 calls `acceptNode`, and reaching that from a C activation is
 * the drive-to-completion this engine aborts on — the filter may loop, await or fork. So the algorithm is a
 * REQUEST SUB-SEQUENCE with its own cursor: the caller embeds a NodeFilterCall, names it in its `visit`, and
 * gets back either "park, and hand this back to me" or the answer. Three of §6.4's steps can suspend and each is
 * a phase of that cursor: the `acceptNode` READ (an accessor or a Proxy trap on a callback-interface object),
 * the CALL, and the RETURN VALUE's `unsigned short` conversion (the page's `valueOf`).
 *
 * THE RE-ENTRANCY GUARD IS THE POINT OF THE `active` FLAG, and it is why it is state rather than a C local: a
 * filter that calls back into the same traverser must throw "InvalidStateError", and the only way to know is a
 * flag that is set ACROSS the suspension the call is. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_FILTER_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* §6.3's acceptNode return values. Named here because §6.1 and §6.2 both branch on them and a second spelling
   of "2" is a walk that rejects where the standard skips. */
#define NODE_FILTER_ACCEPT 1
#define NODE_FILTER_REJECT 2
#define NODE_FILTER_SKIP   3

/* §6's SHARED TRAVERSER STATE. Embedded as the FIRST member of a NodeIterator's and a TreeWalker's record, so
   one set of offsets serves both and TRAVERSER_VALS below can name them. */
typedef struct Traverser {
    JSValue  root;      /* §6 root — the node this traverser is rooted at (a wrapper; OWNED) */
    /* §6 filter — JS_NULL, a callable, or a callback-interface object carrying `acceptNode` (OWNED). */
    JSValue  filter;
    uint32_t what;      /* §6 whatToShow, already through Web IDL's `unsigned long` conversion */
    uint8_t  active;    /* §6 "is active" — set across the acceptNode call, which is what makes re-entry an
                           InvalidStateError rather than an infinite recursion */
} Traverser;

/* THE OWNED SLOTS OF AN EMBEDDED TRAVERSER, for the record's CowRecord — the per-flow COW capture list and the
   finalizer's free list are the same list, and this is the half of it §6 owns. Its argument is the OUTER struct
   so the offsets are the ones the capture will use. */
#define TRAVERSER_VALS(outer) (uint16_t)offsetof(outer, t.root), (uint16_t)offsetof(outer, t.filter)

/* §6.4's CALL, IN FLIGHT. The caller owns one of these, names it in its `visit` and releases it in its
   `release`; nothing here is reachable from anywhere else, so the ownership is exactly this struct. */
typedef struct NodeFilterCall {
    /* §6.4's own cursor. 0 = not started; 1 = parked on Get(filter, "acceptNode"); 2 = parked on the call;
       3 = parked on the return value's conversion. It is a PHASE and not a stage because the traverser member
       that hosts it rests at ONE step of its own algorithm — the step whose filter this is — and the phase says
       which part of §6.4 that filter is inside. */
    uint8_t phase;
    uint8_t cphase;    /* step_call_run's own phase, so the call survives the suspension it is */
    /* THE TRAVERSER WHOSE `is active` THIS CALL SET, and the object that record belongs to (OWNED).
       §6.4 step 6 says the flag is cleared and the exception RETHROWN when `acceptNode` throws — and a throw
       inside the page's callback does not come back through node_filter_run at all: the driver tears the
       machine down and calls its release. So the release is where the standard's sentence lives, and these two
       fields are what lets it: the pointer says which flag, and the reference keeps the record alive to be
       cleared. Both are NULL/undefined until the flag is actually set. */
    Traverser *t;
    JSValue owner;
    JSValue res;       /* the call's result, held between the call and its `unsigned short` conversion (OWNED) */
    JSValue cb[3];     /* the call request's buffer — [this, acceptNode, node] */
} NodeFilterCall;

/* IS A FILTER ALREADY IN FLIGHT ON THIS CALL? §6.4's own cursor answers it, and nothing else can: three of
   §6's loops ADVANCE to the next node and then filter it, so a resume that re-entered the loop from the top
   would advance a second time and skip a node with nothing to say so. A machine parked anywhere inside §6.4 has
   a non-zero phase, and a completed one has released back to zero. */
static inline bool node_filter_in_flight(const NodeFilterCall *c) { return c->phase != 0; }

void node_filter_call_visit(JSContext *ctx, NodeFilterCall *c, JSStepVisit *v);
void node_filter_call_release(JSContext *ctx, NodeFilterCall *c);

/* §6.4 "to filter a node `node` within a traverser". `node` is the node's WRAPPER, because that is what step 6
   hands the page. Returns >0 (the caller returns it unchanged — the machine is parked), 0 with *pres set to one
   of the three FILTER_ values, or -1 with an exception live. The `is active` flag is cleared on every exit,
   including the abrupt one, which is §6.4 step 6's own sentence. */
int node_filter_run(JSContext *ctx, JSStepHdr *hdr, JSValueConst owner, Traverser *t, NodeFilterCall *c,
                    JSValueConst node, JSValue in, int *pres, JSValue **out_cb, int *out_argc);

/* Release the traverser's own two slots — called by each interface's finalizer, which is the same list
   TRAVERSER_VALS names. */
void traverser_release(JSRuntime *rt, Traverser *t);
void traverser_mark(JSRuntime *rt, Traverser *t, JS_MarkFunc *mark_func);

void node_filter_init(JSContext *ctx);
/* §6.3's CALLBACK INTERFACE OBJECT: Web IDL §3.7.2 gives a callback interface with constants a property on the
   global whose value carries them. It is not callable and not constructible — there is nothing to construct. */
void node_filter_install(JSContext *ctx, JSValueConst global);
void node_filter_free(JSContext *ctx);

#endif
