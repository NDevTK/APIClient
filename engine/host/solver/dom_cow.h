/* Per-flow DOM COW delta — the DOM half of the copy-on-write flow isolation (the JS heap half is in quickjs).
 *
 * Every DOM mutation a flow makes (an attribute write, a node insert) is RECORDED into the running flow's
 * delta while capture is on. On a context-switch the scheduler UNAPPLIES the outgoing flow's writes (restoring
 * the shared baseline) and APPLIES the incoming flow's — so any number of flows interleave without seeing each
 * other's DOM writes, and a parked flow's delta rides along as an opaque buffer. Self-contained: it owns the
 * delta buffer and only calls the Lexbor DOM API; the host-edges call the capture hooks, the scheduler calls
 * apply/unapply/buf. */
#ifndef ENGINE_HOST_DOM_COW_H
#define ENGINE_HOST_DOM_COW_H

#include <lexbor/html/html.h>
#include "quickjs.h"

/* Capture gate: while non-zero, DOM mutations are recorded into the running flow's delta. The scheduler sets
   it (0 pre-baseline / during a shared chunk eval, 1 once the per-flow baseline is fixed). */
extern int g_dom_capture;

/* The DOM delta also carries each captured attribute's TAINT SHADOW (attr_shadow) so a source stashed in a DOM
   attribute is isolated per flow EXACTLY like the attribute's value — set once at init so the swap logic can
   dup/free the shadow's opaque JSValues without threading a context through every host-edge/scheduler call. */
void dom_cow_set_ctx(JSContext *ctx);

/* §4.2.3's INSERTION and REMOVING STEPS, fired BY the chokepoint. The browser layer owns what they mean (a
   <script> is prepared, a custom element is upgraded, its disconnected reaction is enqueued); this file owns
   the one place a tree write happens, which is the only place that cannot be forgotten. `inserted` is 1 after
   a node entered the tree and 0 BEFORE one leaves it, because "was it connected" is only answerable then. */
void dom_cow_set_tree_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase));
/* §4.9's ATTRIBUTE CHANGE STEPS, fired by the same chokepoint for the same reason. Called AFTER the write —
   §9.4.6 step 2 stores the value and step 3 handles the change, in that order — so BOTH values are passed and
   the old one is copied across the write rather than read back off an element that no longer has it. `val` is
   NULL for a removal and `old_val` is NULL for an attribute that was absent, which is the null a page's
   attributeChangedCallback branches on.
   THE IDENTITY IS §4.9'S OWN — (namespace, LOCAL name), `ns` NULL for the null namespace. §4.9's "handle
   attribute changes" step 3 passes the namespace to the attribute change steps and step 2 passes it to
   §4.13.3's `attributeChangedCallback` as its FOURTH argument, and DOM's own ID change steps fire only when
   "localName is id AND namespace is null" — so a hook carrying a qualified name alone cannot answer either
   question for a namespaced attribute, and reports an `svg:id` as the element's ID. */
void dom_cow_set_attr_hook(void (*fn)(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                      const char *old_val, size_t old_len, const char *val, size_t val_len));
/* §4.10's "REPLACE DATA" step 4, fired by the third chokepoint for the third thing a flow can change about the
   tree. Called BEFORE the write and handed the node's CURRENT data, because that is the `oldValue` a
   characterData mutation record carries and there is no answer to it afterwards. It is a hook and not a call
   from each of `data=`, `appendData`, `normalize` and `textContent` for the reason the other two are: eleven
   sites write character data and one would remember. */
void dom_cow_set_cdata_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *node, const char *old, size_t old_len));

/* THE TREE VERSION. Every structural change to the document — an insert, a removal, and the SWAP that makes
   one flow's delta the visible tree — advances it. It is what a live collection's index cache is keyed on:
   `for (i = 0; i < el.childNodes.length; i++) el.childNodes[i]` walks the child list once per index without
   one, which is quadratic in the page's own markup and is how a browser's DOM would behave if Blink did not
   carry CollectionIndexCache. Monotonic, so it can only over-invalidate. */
uint64_t dom_cow_version(void);

/* The DOM-mutation CHOKEPOINT — capture-then-mutate ATOMICALLY, in ONE call. A browser component mutates the
   tree ONLY through these, never through raw lxb_dom_* mutators + a separate capture, so a DOM write cannot
   bypass time-travel capture (the browser engineer's "one place to reason about"). This is the fork-free answer
   to unbypassable capture: rather than fork Lexbor to hook its internal write primitives, we OWN the mutation
   API on top of Lexbor and funnel every write through here.
   THIS IS NOW A CONVENTION, NOT AN ENFORCEMENT, and saying so is the point. The line here used to claim it was
   "enforced structurally by engine/check_dom_chokepoint.mjs, which fails the build if a browser component names
   a raw Lexbor mutator at all" — that gate was deleted, and a header that goes on citing a removed check is
   worse than one that admits the gap, because it is what a reader consults INSTEAD of looking. An audit read
   every browser component and found no bypass today: the raw insert/remove/destroy mutators appear only in
   dom_cow.c, and the raw attribute mutators only in attr_list.c, the declared raw layer beneath this one, whose
   single exported entry is the documented parse boundary. That is a measurement of one moment, not a property
   of the build — the first component to reach past this file will do so silently. */
/* VALUE AND TAINT ARE ONE WRITE, and the identity that keys both is resolved HERE. They were two calls every
   caller made in agreement over a key each computed for itself: a caller that made one and not the other left a
   stale taint on a fresh value or dropped the provenance of a stored source, and `toggleAttribute(name, true)`
   did exactly the first. `taint` is JS_UNDEFINED for an ordinary concrete write.
   `name` is a QUALIFIED name — §4.9 "get an attribute by name" is what setAttribute matches with — and the
   attribute it names may live in any namespace; the identity (namespace, local name) is read off the attribute
   the element actually has, so the delta can restore it as the attribute it was. */
void dom_cow_set_attribute(lxb_dom_element_t *el, const char *name, const char *val, size_t val_len,
                           JSValueConst taint);
/* the attribute setter's twin — removeAttribute, and what a boolean reflection does when set to false. The
   taint goes with the value: there is no value left for it to describe. */
void dom_cow_remove_attribute(lxb_dom_element_t *el, const char *name);
/* The same two over §4.9's own key, (namespace, local name), which is what every namespace-aware algorithm is
   defined on. `ns`/`prefix` NULL is the null namespace. */
void dom_cow_set_attribute_ns(lxb_dom_element_t *el, const char *ns, const char *prefix, const char *local,
                              const char *val, size_t val_len, JSValueConst taint);
void dom_cow_remove_attribute_ns(lxb_dom_element_t *el, const char *ns, const char *local);
/* §4.9's NODE-VALUED half — `setAttributeNode`/`setNamedItem` and `removeAttributeNode`/`removeNamedItem`,
   which name the attribute by OBJECT rather than by any name. They are not expressible through the setters
   above: the page's own Attr has to BE the attribute afterwards, so a create would answer `===` wrong. */
void dom_cow_put_attribute_node(lxb_dom_element_t *el, lxb_dom_attr_t *a, JSValueConst taint);
void dom_cow_remove_attribute_node(lxb_dom_attr_t *a);
/* §4.9.2 "create an attribute" — a DETACHED Attr the running flow OWNS, destroyed with its delta. */
lxb_dom_attr_t *dom_cow_create_attribute(lxb_dom_document_t *doc, const char *ns, const char *prefix,
                                         const char *local);
/* §4.9.2 "set an existing attribute value" over an attribute whose element is null. */
void dom_cow_set_detached_attr_value(lxb_dom_attr_t *a, const char *val, size_t val_len,
                                    JSValueConst taint);
/* The taint this attribute carries — BORROWED (dup it to keep), JS_UNDEFINED when it carries none. The read's
   twin of the fused write, resolving the identity the same way so the two cannot disagree. */
JSValue dom_cow_attr_taint(lxb_dom_element_t *el, const char *name);
/* The same read at §4.9's OWN key, which is what the namespace-keyed family (`getAttributeNS`, `Attr.value`)
   asks with — the by-name read above is that question plus the qualified-name resolution in front of it. */
JSValue dom_cow_attr_taint_ns(lxb_dom_element_t *el, const char *ns, const char *local);
/* node-insert chokepoint — the tree-structure twin of dom_cow_set_attribute: capture the insertion THEN attach
   the child, so a subtree a flow appends reverts per-flow (detached on context-switch, re-attached on resume). */
void dom_cow_append_child(lxb_dom_node_t *parent, lxb_dom_node_t *child);
/* the same insert AT A POSITION — §4.2.3 insertBefore, and the reference child's own re-parenting. */
void dom_cow_insert_before(lxb_dom_node_t *ref, lxb_dom_node_t *child);
/* Detach a node the BASELINE may own — captured so it comes back on revert/unapply. innerHTML= needs it:
   it REPLACES children, and an uncaptured removal leaks the old subtree across a context switch. */
void dom_cow_remove_child(lxb_dom_node_t *node);
/* A character-data node's VALUE (§4.10 `data`) — the third thing a flow can change about the tree, on a node
   whose identity must survive the write, so it cannot be a remove+insert of a replacement. */
void dom_cow_set_text(lxb_dom_node_t *node, const char *val, size_t val_len);
/* A DOM PROPERTY's taint (textContent and its kind): the value half is already captured as Text nodes by the
   insert/remove chokepoints, so this captures the taint shadow so it reverts with them. JS_UNDEFINED clears. */
void dom_cow_set_prop_taint(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque);

/* FLOW-PRIVATE TREE OPERATIONS — the DOM's half of the invariant the heap COW already keeps: a delta captures
   only SHARED baseline state, because state the running flow created cannot be observed by another flow and
   capturing it would make the delta O(everything the flow built) rather than O(shared state it touched).
   The fragment parse is where this became load-bearing. `el.innerHTML = markup` parses into a fragment and then
   moves each top-level node into the real tree: the MOVE is a shared write and goes through the chokepoint above,
   but taking the node out of the fragment first, and destroying the emptied fragment after, are writes to a tree
   the parse itself just built and nothing else has ever seen. Routing those through the capturing chokepoint
   would put the fragment's whole internal structure in the delta; calling Lexbor raw is what the check now
   refuses. So they are chokepoint entries that DECLARE the state is private, and ASSERT it.
   THE DECLARATION IS A PARAMETER, not a scope. It was a begin/end pair around a global, and that was wrong the
   moment a MACHINE needed it: a clone of the page's subtree parks in the middle of building the copy, and a
   scope held in a global is open while another flow runs and opens its own.
   NO PREDICATE ON THE NODE WOULD DO. An unparented tree is what a detached parse result and a subtree REMOVED
   from the document both look like, and the second must never be written raw — another flow's baseline still
   holds it. The declared root is checked against every live removal entry, which is what tells them apart. */
void dom_cow_take_private(lxb_dom_node_t *root, lxb_dom_node_t *node);   /* out, on its way to the real tree */
void dom_cow_insert_private(lxb_dom_node_t *root, lxb_dom_node_t *parent, lxb_dom_node_t *child);  /* build it */
void dom_cow_destroy_private(lxb_dom_node_t *root, bool with_children);  /* and drop it */
/* MOVE a node between two trees NEITHER of which is shared — the parse boundary's own operation. HTML
   §13.2.6.4.4 sets a `<template>`'s template contents to the shadow root it attaches, and a parse that filled
   the contents instead has to hand them over: the source is that parse's own product and the destination is a
   shadow root created a statement earlier, so neither end is state another flow can hold. It is NOT
   take_private followed by insert_private — take_private declares the node has LEFT the private tree for the
   real one and makes the flow its owner, and a node that never leaves would then carry a creation entry the
   discard asserts against. BOTH roots are declared, because a declaration this operation could infer is a
   declaration it could infer wrongly. */
void dom_cow_move_private(lxb_dom_node_t *from_root, lxb_dom_node_t *to_root,
                          lxb_dom_node_t *parent, lxb_dom_node_t *child);
/* THE POSITIONAL FORMS, within ONE private tree. Every other operation here appends, because a parse builds in
   order; these two SPLICE, and position is the whole point of the algorithm that needed them — HTML §8.6.4
   step 1.5.2.5 replaces an element with its children IN ITS PLACE, and an append would move them to the end of
   their new parent, reordering the page's markup while claiming only to have removed an element.
   `_insert_before_` takes a child that is in no tree; `_move_before_` takes one already in this same tree,
   which is what step 1.5.2.5 actually has. `ref` must be a node WITH a parent — there is no position before a
   private tree's root — and both assert it. */
/* DOM §4.5 ADOPT's write of a node's NODE DOCUMENT — a chokepoint entry, because a node document is shared
   baseline state exactly like a parent link. It was the one piece of tree state the delta had no kind for, so a
   flow that adopted a subtree moved those nodes into another document for every flow; a sibling arm that never
   adopted read the adopting arm's `ownerDocument`, and everything derived from it followed the wrong document.
   Moves a pointer and destroys nothing — kind 5 is the entry that OWNS a document. */
void dom_cow_set_node_document(lxb_dom_node_t *node, lxb_dom_document_t *doc);

void dom_cow_insert_before_private(lxb_dom_node_t *root, lxb_dom_node_t *ref, lxb_dom_node_t *child);
void dom_cow_move_before_private(lxb_dom_node_t *root, lxb_dom_node_t *ref, lxb_dom_node_t *child);
/* DESTROY one node OF a private tree, in place. §13.2.6.4.4's `<template>` is the caller: the standard never
   inserts it, this engine's parse did, and once its contents are the shadow root's there is nothing that can
   ever reach it again. Not take_private + destroy_private for the reason above — take_private records a
   creation, and freeing what a creation entry names is a use-after-free on the next discard. The node must
   already be empty: everything under it would be freed with it, and this operation exists precisely because
   what was under it went somewhere else. */
void dom_cow_discard_private(lxb_dom_node_t *root, lxb_dom_node_t *node);

/* Lower-level capture primitives the chokepoint is built on (record a mutation BEFORE it happens). Direct use is
   reserved for the mutation ops that compose them (the chokepoint above, and node-insert once it lands). */
void dom_insert_capture(lxb_dom_node_t *node);                    /* an inserted node */

/* Scheduler hooks — swap the running flow's DOM writes. */
void dom_revert(void);      /* DISCARD the running flow's writes -> baseline (flow completed) */
void dom_unapply(void);     /* flow -> parked: stash the flow's values, restore the baseline */
void dom_apply(void);       /* parked -> flow: restore the flow's values over the baseline */

/* Park/resume the delta buffer as an opaque handle (the scheduler stores it on the Flow as void*). */
void *dom_buf_take(int *n, int *cap);          /* detach the current buffer (returns it; delta now empty) */
void dom_buf_load(void *buf, int n, int cap);  /* attach a parked buffer as the current delta */
void dom_buf_free(void *buf, int n);           /* free a parked buffer (its nodes stay owned by the doc) */

/* Persistent-versioned-DOM fork (mirrors the heap's JS_CowFork): freeze the running flow's DOM head into a
   shared immutable base segment (refcount 2) a snapshot-forked sibling references in O(1). The base chain rides
   alongside the head exactly like the heap's cow_base; the scheduler stores it on the Flow as void*. */
void *dom_cow_fork(void);       /* freeze head -> shared base; returns it (the sibling stores the 2nd reference) */
void *dom_base_take(void);      /* detach the shared base chain (park it on the flow) */
void dom_base_load(void *base); /* install a parked flow's base chain (before dom_apply) */
void dom_base_free(void *base); /* drop a flow's reference to a base chain (free iff last) */
void dom_base_ref(void *base);  /* add ONE ref (each orphan forks the document flow's shared DOM delta) */

/* WHAT THE DOCUMENT'S CHAIN IS HOLDING RIGHT NOW — frozen segments still referenced, and the entries in them.
   The heap half reports the same pair (cow.h); they are read together because a delta nobody released looks the
   same from either side and only the one that CLIMBS names which half owns it. */
void dom_cow_chain_stats(long *segs, long *entries);
/* …in the unit the cold tier pages in, and the same for ONE flow's parked head at capacity `cap`. Asked of
   this file for the reason cow.h's twin is: `sizeof(DomUndo)` is private and a caller that guessed it would
   report a number that drifts the next time an entry kind is added. */
long dom_cow_chain_bytes(void);
long dom_cow_head_bytes(int cap);

/* THIS FLOW CREATED THIS NODE. Called from every place a node is made, so the delta owns it and destroys it
   when the delta is discarded. Inert while capture is off — a boot-time creation is baseline and outlives
   every delta. */
void dom_cow_note_created(lxb_dom_node_t *node);

/* THIS FLOW CREATED THIS DOCUMENT — DOM §4.5.1's createHTMLDocument and createDocument, whose result is a whole
   second Document rather than a node in one. Same contract as dom_cow_note_created and one difference that has
   to come back to the caller: a baseline NODE is owned by the document it was made in, and a document has no
   such parent. So this RETURNS true when the running flow's delta took ownership (it destroys the document when
   the delta is discarded) and false when capture is off, which means the creation is BASELINE and the realm
   that made it is what must own it. */
bool dom_cow_note_created_document(lxb_html_document_t *dom);

/* DESTROY A DOCUMENT THIS ENGINE OWNS, wrappers first. The identity map holds a strong reference per node, so a
   document freed without this leaves the map naming freed memory — which a pool allocator turns into the next
   node inheriting a dead wrapper — and leaves the runtime's own leak walk counting its whole tree. */
void dom_cow_destroy_document(lxb_html_document_t *dom);

#endif
