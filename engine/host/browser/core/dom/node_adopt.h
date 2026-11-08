/* DOM §4.5 "Interface Document"'s adopt, its steps 1 and 3, as a walk a step machine takes one node per step;
 * core/dom/node.c implements it. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_ADOPT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_ADOPT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* DOM §4.5 adopt's steps 1 and 3 as a walk one shadow-including inclusive descendant per step, for a step machine
   that ran step 2's removal itself. A frame is one invocation of adopt (step 3.4 adopts a template's contents
   into another document as a nested one); the state owns `f`, visited by node_adopt_visit. */
typedef struct NodeAdoptFrame {
    lxb_dom_node_t     *root;   /* this invocation's node, which bounds its shadow-including walk */
    lxb_dom_node_t     *cur;    /* the next inclusive descendant step 3 has still to visit */
    lxb_dom_document_t *doc;    /* this invocation's document */
    lxb_dom_document_t *old;    /* step 1's oldDocument */
} NodeAdoptFrame;
typedef struct NodeAdoptState {
    NodeAdoptFrame *f;
    int sp, cap;
} NodeAdoptState;
/* Steps 1 and 3's condition. `ctx` is the realm of the document adopted into, and may be NULL only when step 3's
   condition skips the walk. Asserts node has no parent and `s` holds no walk. */
void node_adopt_start(JSContext *ctx, NodeAdoptState *s, lxb_dom_node_t *node, lxb_dom_document_t *document);
/* Step 3 for one descendant, charged as one DOM-walk unit; false once the walk is done (its frames freed). */
bool node_adopt_step(JSContext *ctx, NodeAdoptState *s);
void node_adopt_visit(JSContext *ctx, NodeAdoptState *s, JSStepVisit *v);

#endif
