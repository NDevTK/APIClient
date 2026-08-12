/* THE NODE INTERFACE — DOM §4.4, the base every tree object shares. One JS object per Lexbor node. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"
#include "quickjs-step.h"

void node_init(JSContext *ctx);
/* §4.4's prototypes for ONE realm — Node, CharacterData, Text, Comment. Declared into core/realm.h's list. */
void node_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue node_chardata_proto(JSContext *ctx);
void node_free(JSContext *ctx);

/* The wrapper for `n`, or JS_NULL. The SAME Lexbor node always yields the SAME JS object: a page compares nodes
   by identity constantly, and a fresh wrapper per lookup makes every such comparison silently false. The
   prototype is chosen by the node's TYPE, so a Text node is a Text and an Element is an Element. */
JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n);
/* The wrapper this node ALREADY has, or JS_UNDEFINED — node_wrap's lookup without node_wrap's allocation, for a
   component whose question is answered by a slot ON the wrapper and must not mint one to ask. BORROWED. */
JSValueConst node_wrap_peek(const lxb_dom_node_t *n);
/* The Lexbor node behind any wrapper, or NULL if `v` is not one. */
lxb_dom_node_t *node_of(JSValueConst v);
/* §4.7: A ShadowRoot IS a DocumentFragment. Lexbor gives a shadow root its own node type — which is what makes
   "is this a shadow root" answerable with no realm in hand — so every rule the standard states over a
   DocumentFragment asks THIS rather than comparing the type, and a rule about the shadow root itself asks
   shadow_root_is (core/dom/shadow_root.h). */
bool node_is_document_fragment(const lxb_dom_node_t *n);
/* Node.prototype — the base a derived DOM interface inherits from, borrowed. */
JSValue node_proto(JSContext *ctx);
/* The interface a node TYPE wears, borrowed — what a component naming its own interface object reads. */
JSValue node_type_proto(JSContext *ctx, int node_type);
/* A derived interface claims the node type(s) it is the interface OF: element.c claims ELEMENT with the
   Element.prototype it built on top of node_proto(). `proto` is CONSUMED — the table owns what it holds, and
   claiming a type twice is a DCHECK because one of the two would silently lose. This is what keeps node_wrap
   the ONE place a wrapper is built: two builders is two identity tables, which is no identity at all. */
/* A DERIVED INTERFACE CLAIMS A NODE TYPE, once per AGENT: the table maps a type to the CLASS whose
   per-context proto slot holds that realm's prototype. The prototype itself is never registered here — it is
   per realm, and the claiming component installs it into its own slot. */
void node_claim_type(int node_type, JSClassID cls);
/* Install `Node`, `CharacterData`, `Text` and `Comment` as globals — the interface OBJECTS, carrying §4.4's
   constants and the prototypes an `instanceof` names. */
void node_install_interfaces(JSContext *ctx, JSValueConst global);
/* One interface OBJECT for a component that owns a derived interface — element.c installs `Element`,
   document.c installs `Document`. Not constructible (none of these declares a constructor), inheriting Node's
   interface object so §4.4's constants read off it. */
void node_install_interface(JSContext *ctx, JSValueConst global, const char *name, JSValueConst proto);
/* THE SAME INSTALL FOR AN INTERFACE THAT HAS A REAL CONSTRUCTOR. Every interface object above shares one
   "Illegal constructor" throw because none of those interfaces declares one — but HTMLElement's IDL carries
   `[HTMLConstructor]`, which is HTML §4.13.2, a fifteen-step algorithm that reads `Get(NewTarget, "prototype")`
   off the page's class. So the caller mints the machine and this hangs it where the shared throw would go,
   with the same prototype pairing and the same Node-interface inheritance. `ctor` is CONSUMED. */
void node_install_interface_ctor(JSContext *ctx, JSValueConst global, const char *name, JSValueConst proto,
                                 JSValue ctor);
/* The class id every node wrapper shares — components that need JS_NewObjectClass for one. */
JSClassID node_class_id(void);
/* What to do with a node once it is inserted (a <script> is PREPARED per HTML 4.12.1) — element.c's rule, asked
   for here so the base does not have to know what a script is. */
/* §4.2.3's INSERTION and REMOVING STEPS. Registered by the element component, fired by the DOM-mutation
   chokepoint itself, so a tree write cannot reach the tree without them — the same structural guarantee the
   chokepoint already gives time-travel capture. `inserted` is 1 for a node that entered a document and 0 for
   one that is about to leave it; the callee walks the SUBTREE, because inserting a subtree connects every
   element in it. */
/* §4.2.7 ChildNode and §4.2.8 ParentNode — the convenience mixins, installed on the interfaces whose IDL
   INCLUDES them. Not on Node.prototype: `document.remove()` is not a member of anything. */
void node_install_child_mixin(JSContext *ctx, JSValueConst proto);
/* §4.2.4 NonElementParentNode — getElementById, on Document and DocumentFragment. One implementation over its
   RECEIVER, for the reason ParentNode's members are: a mixin is what the IDL says these are. */
void node_install_nonelement_parent_mixin(JSContext *ctx, JSValueConst proto);
void node_install_parent_mixin(JSContext *ctx, JSValueConst proto);
/* §4.2.3's insertion/removing steps, as the LIST the standard writes: a component REGISTERS one and the
   chokepoint runs them all, in registration order.
   THREE PHASES, not two, because §4.2.3's `remove` has work on BOTH sides of the detach and one of them cannot
   be moved. Steps 1-2's live-range and NodeIterator pre-remove steps read the node's position and must run
   BEFORE it leaves; steps 4-7's SLOT steps recompute a slot's assigned nodes and must run AFTER, or the
   recomputation still finds the node being removed and nothing changes — so the removal fires no `slotchange`.
   `parent` is the node's parent, PASSED rather than read off the node, because by NODE_TREE_REMOVED there is
   none left to read. */
enum { NODE_TREE_REMOVED = -1, NODE_TREE_REMOVING = 0, NODE_TREE_INSERTED = 1 };
typedef void (*NodeTreeHook)(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase);
void node_add_tree_hook(NodeTreeHook fn);
/* THE PRE-ORDER SUCCESSOR within `root`'s subtree, or NULL at the end — the one traversal primitive the spec's
   tree-order algorithms need. Exported for the same reason it exists: having it once is what keeps every
   algorithm that walks in tree order from growing its own walker that disagrees at the edges. */
lxb_dom_node_t *node_next_in(lxb_dom_node_t *n, lxb_dom_node_t *root);
/* Its inverse — the pre-order PREDECESSOR within `root`'s subtree, or NULL at the beginning. §6.1's backwards
   traversal is stated over exactly this, and a second one written inline gets the last-descendant descent
   wrong. */
lxb_dom_node_t *node_prev_in(lxb_dom_node_t *n, lxb_dom_node_t *root);
/* §4.4 isConnected — is this node's root a document. */
bool node_is_connected(const lxb_dom_node_t *n);
/* §4.4's "root": the topmost inclusive ancestor. §5's boundary points and §6's traversers both ask. */
lxb_dom_node_t *node_root(lxb_dom_node_t *n);
/* §4.2's "inclusive ancestor" — walked UP from the descendant, so it is O(depth) with no allocation. */
bool node_is_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b);
/* §4.4's "length" — 0 for a doctype, the data length of a CharacterData node, the child count otherwise. */
uint32_t node_length(const lxb_dom_node_t *n);
/* §4.2's "index" — how many siblings precede this node. */
uint32_t node_index(const lxb_dom_node_t *n);
/* §4.10's "REPLACE DATA" — the concept, with its own IndexSizeError and its own count clamp, and it runs §5.5's
   live-range steps 8-11 on the way out. §5.5's content-moving members are stated over it by name; a second
   splice written beside it is a second place for those steps to be forgotten. `offset` and `count` are in CODE
   UNITS, `data`/`data_len` are UTF-8 bytes. */
JSValue node_cd_replace_data(JSContext *ctx, lxb_dom_node_t *n, uint32_t offset, uint32_t count,
                             const char *data, size_t data_len);
/* THE BYTE OFFSET at which code-unit offset `units` begins in a CharacterData node's stored UTF-8. §4.4's
   length is in CODE UNITS and lexbor stores UTF-8, so every §5 algorithm that touches the bytes behind an
   offset goes through this one walk rather than growing a second that disagrees at a surrogate pair. */
size_t node_cd_byte_of(const lxb_dom_node_t *n, uint32_t units);
/* §4.11's "split a Text node" — the CONCEPT, which §5.5's `insertNode` is stated over. Returns the second half,
   or NULL having thrown "IndexSizeError". It carries the live-range steps a substringData+insertBefore
   composition cannot. */
lxb_dom_node_t *node_split_text(JSContext *ctx, lxb_dom_node_t *node, uint32_t offset);
/* §4.2.3's "insert", as this engine's ONE tree-write helper: a DocumentFragment contributes its CHILDREN, a
   node already in a tree is removed from it first, and `ref` naming `node` itself resolves to its next sibling.
   Exported because §5.5's content-moving members are all stated over pre-insert and append; a second copy of
   the fragment rule is how fragments quietly stop working. `ref` NULL appends. */
void node_insert_at(lxb_dom_node_t *parent, lxb_dom_node_t *node, lxb_dom_node_t *ref);

/* DOM §4.4 "CLONE A NODE" — THE ALGORITHM, DELEGATABLE, so the one implementation is the one every caller runs.
 *
 * §5.5's extract and clone-the-contents are stated over it in six places ("let clone be a clone of
 * originalStartNode", "a clone of contained child with subtree set to true"), and a caller that copies a node
 * itself instead is not a shortcut — it is a DIFFERENT algorithm with step 3 (the cloning steps HTML defines for
 * `input`, `textarea`, `script` and `template`) and step 6 (a clonable shadow root, which is cloned even when
 * subtree is false) missing from it, silently, per node. So the algorithm is exported rather than the member.
 *
 * A CALLER PERFORMS IT INSIDE ITS OWN MACHINE, which is what makes it parkable at every node of a subtree the
 * page chose the depth of: declare the six stages inside the caller's own stage block with
 * NODE_CLONE_ALGO_STAGES, embed a NodeCloneState, chain node_clone_visit_state / node_clone_release_state into
 * the caller's visit and release, and hand node_clone_run the base of that block. Stage identity is the LABEL,
 * so the list is expanded once per caller with that caller's own leading text and its own prefix.
 *
 * THIS IS THE `parent`-NULL FORM. §4.4's step 4 ("if parent is non-null, then append copy to parent") is what
 * the walk does for every DESCENDANT; both callers of the entry are stated over a null parent and append the
 * copy where their own steps say to — cloneNode returns it, §5.5 appends it to the fragment. */
#define NODE_CLONE_ALGO_STAGES(X, P, W) \
    X(P##_ROOT,     W " → DOM §4.4 clone a node steps 1-2 (clone a single node: the root of the copy)") \
    X(P##_COPY,     W " → DOM §4.4 clone a node steps 2 and 4 (clone a single node; append copy to parent), " \
                      "one descendant per step") \
    X(P##_TEMPLATE, W " → DOM §4.4 clone a node step 3 (HTML §4.12.3 the template element's cloning steps)") \
    X(P##_CHILDREN, W " → DOM §4.4 clone a node step 5 (for each child of node's children, in tree order)") \
    X(P##_SHADOW,   W " → DOM §4.4 clone a node step 6 (a shadow host's clonable shadow root: steps 6.1-6.7 " \
                      "attach it onto the copy, 6.8 clones its children), after step 5") \
    X(P##_LEAVE,    W " → DOM §4.4 clone a node step 7 (return copy): this node's clone is complete, and step " \
                      "5's loop over its parent's children advances")

/* A LEVEL of the walk — a tree reached OTHER THAN through child links, which is a `<template>`'s content
   fragment (HTML §4.12.3's cloning steps) and a shadow tree (§4.4 step 6.8). `stage` is where the level that
   pushed this frame RESUMES, and it is not the same for the two: a template's content is cloned by step 3, so
   the element's own step 5 is still to come, while a shadow tree is cloned by step 6, after which only step 7
   remains. A frame with no resume stage is what made step 6 unimplementable — popping back always meant
   "children next", so a shadow descent would have re-walked the light children it was standing after. */
typedef struct NodeCloneFrame {
    lxb_dom_node_t *src, *dst, *root, *croot, *cnode;
    bool deep;
    int  stage;
} NodeCloneFrame;

typedef struct NodeCloneState {
    lxb_dom_node_t *src;     /* the cursor in the original */
    lxb_dom_node_t *root;    /* what bounds the CURRENT level of the walk */
    lxb_dom_node_t *dst;     /* the copy the next child is inserted into */
    lxb_dom_node_t *copy;    /* the copy's root — the ANSWER, read by the caller at its resume stage */
    /* §4.4's `document` ARGUMENT: the document every "clone a single node" creates its copy in. It is node's
       node document for every ordinary clone and the COPY for a document ("if node is a document, then set
       document to copy"), which is the whole of what makes a cloned Document's tree belong to the clone. */
    lxb_dom_document_t *doc;
    /* THE PRIVATE-TREE DECLARATION FOR THE CURRENT LEVEL, which is not always `copy`. A `<template>`'s content
       is a SEPARATE tree — reached through the element's `content` field, not through child links — so once the
       walk descends into it, the tree being built into is that fragment. It is private for the same reason the
       copy is: clone_interface made it a moment ago and nothing has ever seen it. A cloned shadow tree is the
       second such tree, reached through §4.9's association instead. */
    lxb_dom_node_t *croot;
    lxb_dom_node_t *cnode;   /* the copy of `src` — what its own children get inserted under */
    /* `clone a node`'s `subtree` FOR THE CURRENT LEVEL, which is not one value for the whole call: step 5 is
       conditioned on it, HTML §4.12.3 returns early without it, and step 6.8 passes TRUE regardless — so
       `host.cloneNode(false)` clones no light children and the whole shadow tree. */
    bool deep;
    int  after;              /* the CALLER's stage this algorithm hands control back at, with `copy` filled */
    NodeCloneFrame *stack;   /* the levels above this one */
    int sp, scap;
} NodeCloneState;

/* Begin `clone a node` given `node` and `subtree`. `base` is where the caller declared the algorithm's stage
   block and `after` is the caller's own stage it resumes at with `s->copy` filled in. Every field the walk
   reads is placed here, before the first step that can allocate or throw. */
void node_clone_start(JSStepHdr *hdr, NodeCloneState *s, lxb_dom_node_t *node, bool subtree, int base, int after);
/* ONE STAGE of it. JS_STEP_YIELD to rest, JS_STEP_ABRUPT having thrown; the finish sets `hdr->stage` to the
   caller's `after`, so a caller never tests for completion. */
int  node_clone_run(JSContext *ctx, JSStepHdr *hdr, NodeCloneState *s, int base);
/* The caller's own visit and release chain into these — the level stack is this state's allocation. */
void node_clone_visit_state(JSContext *ctx, NodeCloneState *s, JSStepVisit *v);
void node_clone_release_state(JSContext *ctx, NodeCloneState *s);
/* An ELEMENT's interface is decided by its TAG — HTML's element-interface table, which is the html layer's
   knowledge. It registers the answer here; node_wrap asks it and stays the one place a wrapper is built. */
void node_set_element_resolver(JSValue (*fn)(JSContext *ctx, lxb_dom_element_t *el));

/* THE NODE IS BEING DESTROYED — drop the wrapper the map holds for it. Called from the DOM's destroy
   chokepoint, which is the only place a node's lifetime ends, so the map stays bounded by the nodes that
   actually exist rather than by every node ever created. */
void node_wrap_forget(JSContext *ctx, lxb_dom_node_t *n);

/* The wrapper identity map's size: how many nodes it names and how many slots it has. Reported by the seam
   assertion because a table that has grown out of proportion to the live document is the shape of a leak. */
void node_wrap_stats(long *n, long *cap);

#endif
