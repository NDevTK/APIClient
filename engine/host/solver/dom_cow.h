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
#include "quickjs-step.h"   /* JSStepTreeOps — a private tree is a thing a step machine's fork has to copy */

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
   of the build — the first component to reach past this file will do so silently.
   AND THE AUDIT'S SCOPE IS THE ENGINE'S OWN COMPONENTS, WHICH IS NOT EVERY WRITER OF THIS TREE. HTML §13.2.6
   "Tree construction" runs INSIDE lexbor and writes the document without one call to this file. A grep of
   `engine/host/browser` cannot see it, which is why it is not a convention there at all: the vendored parser
   routes those writes through one interposable table, this file implements it, and each parse DECLARES whose
   tree it is building (see dom_cow_parse_declare below) so a write into the page's own document is captured
   and a write into a parse's own private tree is not.
   AND LEXBOR'S OWN MUTATION CALLBACKS DO NOT CLOSE IT — a callback that FIRES is not a write that is CAPTURED,
   and the shapes differ in a way that matters here rather than pedantically:
     - `->mutation->inserted` fires AFTER the link, which is FINE for an insert: a kind-1 entry holds only the
       node and reads its position at unapply time, so a linked node is exactly what it needs. But it is
       invoked over every SHADOW-INCLUDING DESCENDANT of the inserted subtree, and an entry per descendant is
       wrong twice — N entries for one write, and an unapply that detaches a descendant whose ancestor has
       already gone records a position inside a detached tree. The arrivals are root-first pre-order, so the
       top node is the first of a burst; the callback carries no burst identity to say so, and the only
       set-free way to tell is to test each arrival's ancestry against the entry just pushed.
     - `->mutation->removed` fires AFTER `lxb_dom_node_remove_wo_events`, which NULLs `parent`, `next` and
       `prev`. It is handed `(node, old_parent)` — so the parent survives and THE OLD NEXT SIBLING DOES NOT. A
       kind-2 entry built from it restores the node as its parent's LAST child instead of at the index the
       baseline had. That is not a gap to fill, it is the wrong shape: the position has to be read BEFORE the
       unlink, which is what dom_cow_remove_child does and what no after-the-fact hook can.
   Those callbacks are DOM §4.2.3's insertion/removing STEPS, which this file already routes through
   dom_cow_set_tree_hook. They are the right home for the steps and they are not the capture. */
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
/* DOM §4.2.3's "MOVE", AS ITS OWN CHOKEPOINT PAIR — steps 13 and 19-20 of `move a node node into a node
   newParent before null or a node child`. The standard says why this is not remove-then-insert in its own
   words, in the note under move step 24.2: "Because the move algorithm is a separate primitive from insert
   and remove, it does not invoke the insertion steps or removing steps for inclusiveDescendant." Those steps
   are what destroy a child navigable's document, reset a focused area, hide a popover and re-fire a custom
   element's connected/disconnected pair — every piece of state `moveBefore()` exists to preserve. So a move
   that reached `dom_cow_remove_child` + `dom_cow_insert_before` would be a state-PRESERVING member built out
   of the two calls that destroy the state, and nothing would say so: the tree would end up right and the
   iframe would be blank.
   THE DELTA ENTRIES ARE EXACTLY THE TWO A REMOVE AND AN INSERT PUSH, which is what makes a move time-travel
   with no new entry kind: the removal remembers (old parent, old next sibling) and the insertion remembers
   where it landed, so an unapply walks them head-first and puts the node back where the baseline had it.
   IT IS A PAIR AND NOT ONE CALL because §4.2.3 runs steps 14-16's slot steps BETWEEN them, on a node that is
   already detached — slot_removed_steps asserts that detachment. The two halves are one uninterrupted C
   operation (nothing between them runs the page's code), and in dev the second asserts that the first ran for
   the same node, because a `_move_out` with no `_move_in` is a node removed with no removing steps. */
void dom_cow_move_out(lxb_dom_node_t *node);
void dom_cow_move_in(lxb_dom_node_t *parent, lxb_dom_node_t *node, lxb_dom_node_t *ref);
/* A character-data node's VALUE (§4.10 `data`) — the third thing a flow can change about the tree, on a node
   whose identity must survive the write, so it cannot be a remove+insert of a replacement. */
void dom_cow_set_text(lxb_dom_node_t *node, const char *val, size_t val_len);
/* HTML §13.2.6.1 "Creating and inserting nodes"'s "insert a character" step 3 — "If there is a Text node
   immediately before insertionLocation, then append data to that Text node's data".
 *
 * IT IS ITS OWN ENTRY BECAUSE THE STANDARD MAKES IT ONE, not because tree construction is a special caller.
 * That step writes DOM §4.10 "Interface CharacterData"'s `data` CONCEPT directly — the spec's own link — and
 * NOT §4.10's "replace data" algorithm, whose step 4 queues a "characterData" mutation record and whose steps
 * 7-10 move every live range that straddles the write. So this fires no cdata hook and touches no range: an
 * entry that routed the merge through dom_cow_set_text would report a mutation the page must not observe and
 * move ranges the parser must not move. The two are the same DELTA record (a character-data node's whole
 * value, restored over the node) and two different DOM operations.
 *
 * AND UNAPPLYING IT IS NOT DELETING A NODE, which is the whole reason it cannot ride the insert entry that
 * covers every other thing tree construction does. The merge target is a Text node that EXISTED before the
 * write — §13.2.6.1's own worked example is "A<script>…document.body.appendChild(text)…</script>C", which the
 * standard annotates "the parser appends to the Text node created by the script" — so the node is not the
 * flow's to remove and the append is not a creation to undo. What reverts is the node's VALUE: the entry holds
 * the bytes the node had BEFORE the append, and an unapply writes them back over the whole of the node's
 * current data, which truncates the appended tail with the node's identity, its parent and its position
 * untouched. N merges into one Text node push N entries holding progressively longer prefixes; the head is
 * unapplied newest-first, so it walks back down to the baseline, and applied oldest-first, so it walks back up
 * to the flow's value. There is no dedup — "this entry says the same thing as an earlier one" is a seen-set,
 * and the cost is one entry per MERGE rather than per node.
 *
 * THE ARENA IS THE NODE'S OWN DOCUMENT'S TEXT ARENA, which is what `lexbor_str_append` in §13.2.6.1 used and
 * what `lxb_dom_character_data_replace` reaches for on the restore, so the capture and its undo allocate out
 * of one place. */
void dom_cow_append_text_data(lxb_dom_node_t *node, const char *data, size_t len);

/* HTML §13.2.6 "Tree construction" — WHOSE TREE A PARSE IS BUILDING, DECLARED BY WHOEVER OPENS THE PARSE.
 *
 * §13.2.6's writes arrive at this file with ONE piece of context: the `lxb_html_tree_t` doing the parsing. The
 * question every one of them must answer first — is this tree one no other flow can reach, or is it the shared
 * baseline every flow reads — IS NOT ANSWERABLE FROM THAT TREE, from the document on it, or from any node in
 * it. A Document node is unparented whether the page holds it or the caller made it a statement ago, so the
 * private-tree predicate says the same thing about both. `lxb_html_document_is_original` — the question these
 * members used to ask — is false ONLY for §13.4 "Parsing HTML fragments" step 3's temporary document, so it
 * called every scratch Document a flow builds (§8.5.1's DOMParser, §4.5.1's createHTMLDocument, XHR's
 * responseXML, an @S witness parse) the page's own and refused all of them.
 *
 * SO THE PARSE DECLARES IT, AND THE DECLARATION IS ABOUT THE OPERATION RATHER THAN ABOUT THE NODE. "No other
 * flow can reach this root" holds because the caller CREATED the Document and opened the parse as one
 * uninterrupted C operation: nothing between the two runs the page's code, so no fork can have produced a
 * sibling that holds it. That is a fact only that caller has, which is why no predicate here recovers it — and
 * why a guess would be silent rather than loud. Guessing PRIVATE for a shared tree writes the page's own
 * document with no delta entry, and every sibling flow then reads a parse it never ran.
 *
 * IT IS PER PARSE AND NOT PER DOCUMENT, for that same reason. A document's disposition is not fixed for its
 * life: a flow that creates a Document, FORKS, and only then parses into it has a sibling holding that
 * document, and a standing per-document claim would go on saying "private". A declaration lives exactly as long
 * as the parse it was made for — made where the parse opens, released at §13.2.7 "The end" — so it cannot
 * outlive the operation that vouched for it.
 *
 * AND IT IS NOT A SCOPE, for the reason the private-tree entries below are not: §13.4's fragment parse yields
 * between BYTES, so a declaration held in a global would stand open while another flow ran and opened its own.
 * Keyed on the parse, there is nothing for a suspension to survive. */
typedef enum {
    /* The root is the running flow's OWN product and no other flow can reach it — §13.4's temporary document,
       and any Document the declaring operation created. Structural writes push NO entry, which is what keeps a
       delta O(shared state touched) rather than O(everything the parse built). */
    DOM_PARSE_ROOT_PRIVATE = 0,
    /* The root is the SHARED baseline every flow reads — the ACTIVE document, whose parse §8.4.3
       `document.write()` step 11 re-enters. Structural writes are captured. */
    DOM_PARSE_ROOT_SHARED = 1
} DomParseRootKind;

void dom_cow_parse_declare(lxb_html_tree_t *tree, lxb_dom_node_t *root, DomParseRootKind kind);
/* …and the parse is over. Takes only the tree, and dereferences nothing else: §13.4's temporary document is
   DESTROYED by `lxb_html_parse_fragment_chunk_end`, so by the time a fragment parse can say it has finished,
   the root it declared is already freed memory. */
void dom_cow_parse_release(lxb_html_tree_t *tree);

/* …AND INSTALL THIS FILE AS THE PARSER'S DOM WRITER. §13.2.6 runs inside lexbor and writes the document
 * without one call to the chokepoint above. The vendored parser makes every one of those writes through a
 * single interposable table (`lxb_html_tree_dom_cb_t`, html/tree.h) instead of through the raw `_wo_events`
 * primitives and a `lexbor_str_append` into a Text node's storage, and this installs the implementation.
 * The character merge is the one member that captures whatever the declaration says — see
 * dom_cow_append_text_data.
 * FOUR OF THE FIVE MEMBERS ARE MUTATIONS AND THE FIFTH IS NOT. `create` is called where §13.2.6 MAKES a node,
 * before it is anywhere, and it is what makes a SHARED parse safe to run inside a flow: the mutation members
 * say what to put back when a delta is unapplied, and only the creation record says what to DESTROY when that
 * delta is discarded. Without it a `document.write()` into the page's own tree left every node it built
 * detached by the undo and freed by nobody. It cannot be folded into `insert_child` — §13.2.6.4.7's adoption
 * agency re-inserts one node several times, and two ownership records that each destroy it is a double free.
 *
 * IT DOES NOT FIRE THE TREE HOOK, and that is a statement about §4.2.3's insertion and removing steps rather
 * than about capture. The hook this file fires is THIS ENGINE's §4.2.3 steps and the chokepoint above is the
 * only thing that reaches it; the table a parsed node's insert does reach is lexbor's own per-document
 * `mutation`, whose members are lexbor's per-tag element steps and not the engine's. So a parser-inserted node
 * runs no §4.2.3 insertion steps here, and the members below leave that exactly where it was. Firing the hook
 * from HERE would close it for a SHARED parse and not for a private one — one parse behaving two ways
 * according to a declaration that is about isolation and says nothing about the DOM's steps. */
void dom_cow_install_tree_construction(void);
/* Whether that install has happened and still stands — asked by the parse entry on every parser it makes, and
   a POSITIVE statement rather than a hole: a §13.2.6 write reaching lexbor's own table is a write no delta
   can ever see, and there is no later point at which that becomes visible. */
bool dom_cow_owns_tree_construction(void);
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
   holds it. The declared root is checked against every live removal entry, which is what tells them apart.
   AND THERE IS ONE OWNERSHIP CONVENTION OVER A PRIVATE TREE, WHICH EVERY PARSE THAT FEEDS ONE KEEPS. A parse
   declared DOM_PARSE_ROOT_PRIVATE records NO creation: the whole tree has ONE owner, which is the private root
   — a Document the declaring operation made (the delta owns it as a created DOCUMENT) or a fragment the parse
   destroys — and a node acquires its OWN owner at exactly one seam, dom_cow_take_private, because leaving the
   private root is the moment that root stops being able to free it. A parse that instead records per created
   node and is then placed node-by-node claims every placed node TWICE, and the discard's second destroy runs
   over memory the first one freed. dom_cow_note_created asserts against it rather than leaving it to be kept. */
/* OUT of the private root, and the one seam at which a node of a private tree gets its OWN owner — the flow's
   delta, which destroys it on discard. Usually on its way to the real tree (the capturing insert that follows
   is the write another flow could see); also on its way to the flow's own discard, which is HTML §8.5.1 step
   3's "Assert: document has no child nodes" emptying an ill-formed XML parse's partial tree. Both are the same
   fact — the root can no longer free it — and that is what the record states. */
void dom_cow_take_private(lxb_dom_node_t *root, lxb_dom_node_t *node);
void dom_cow_insert_private(lxb_dom_node_t *root, lxb_dom_node_t *parent, lxb_dom_node_t *child);  /* build it */
/* and drop it — `with_children` false is an emptied root and asserts it; true DEEP-destroys, which is the
   abandoned-parse case (a machine torn down mid-placement still holds everything it has not moved yet). */
void dom_cow_destroy_private(lxb_dom_node_t *root, bool with_children);
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

/* A PRIVATE TREE, DECLARED TO A STEP MACHINE'S FORK AND TEARDOWN — quickjs-step.h's `v->tree`, answered here
   because the DESTROY half is `dom_cow_destroy_private` two declarations up and the CLONE half is the same
   question asked forwards: what does the sibling arm get that neither arm can free twice.
   IT BELONGS ON THE DECLARATION AND NEVER IN A `release`. A machine's `visit` is the ONE list of what it owns,
   so a tree named here is cloned by the fork and freed by the teardown through that one list — and a `release`
   that also freed it is the second list, which the teardown's own fingerprint bracket now measures rather than
   trusts. That is the whole reason this is an operation the engine dispatches instead of a pair of calls a
   machine makes for itself: a machine must never learn which consumer is visiting it.
   THE CURSORS ARE THE OTHER HALF AND THEY ARE NOT OPTIONAL. Every pointer a machine holds INTO the tree — a
   placement cursor, the parse context when the parse created it, a walk standing on a node — names a node the
   copy replaced, so a clone that re-pointed the root and nothing else hands the sibling a tree it owns and a
   set of pointers into the arm it was forked from. */
extern const JSStepTreeOps dom_cow_private_tree_ops;

/* Scheduler hooks — swap the running flow's DOM writes. There is no `dom_revert` twin that DISCARDS them: a
   flow that finishes is switched out like any other and then RELEASED, which is the same path an evicted flow
   takes (see the note where dom_revert used to be, and flow_release in solver/flow.h). */
void dom_unapply(void);     /* flow -> parked: stash the flow's values, restore the baseline */
void dom_apply(void);       /* parked -> flow: restore the flow's values over the baseline */

/* Park/resume the delta buffer as an opaque handle (the scheduler stores it on the Flow as void*). */
void *dom_buf_take(int *n, int *cap);          /* detach the current buffer (returns it; delta now empty) */
void dom_buf_load(void *buf, int n, int cap);  /* attach a parked buffer as the current delta */
/* Free a PARKED buffer — the head's entries and the nodes the flow CREATED, which die with it. It must be a
   parked one: the creations are destroyed deep, and a head still applied has them in the tree (asserted where
   they are released). Call it BEFORE dom_base_release: a head node inserted under a segment's node is that
   node's child, and a child must be freed before the parent it hangs under is freed deep. */
void dom_buf_free(void *buf, int n);

/* Persistent-versioned-DOM fork (mirrors the heap's JS_CowFork): freeze the running flow's DOM head into a
   shared immutable base segment (refcount 2) a snapshot-forked sibling references in O(1). The base chain rides
   alongside the head exactly like the heap's cow_base; the scheduler stores it on the Flow as void*. */
void *dom_cow_fork(void);       /* freeze head -> shared base; returns it (the sibling stores the 2nd reference) */
void *dom_base_take(void);      /* detach the shared base chain (park it on the flow) */
void dom_base_load(void *base); /* install a parked flow's base chain (before dom_apply) */
/* Drop a flow's reference on a base chain (freed iff last), bringing the DOCUMENT back down to the deepest
   segment that survives it first — never further. The heap twin is cow_delta_release and the reason is the
   same one twice: `g_dom_installed` holds no reference, so a dying segment can be the one the document is
   showing, and here that is not a dangling pointer but the nodes the segment created being destroyed while
   they are still live tree. */
/* A SEGMENT'S REFCOUNT IS EXACTLY the number of flows whose `dom_base` names it plus the number of segments
   whose `base` names it, and there is deliberately no `dom_base_ref` to raise it from outside: dom_cow_fork is
   the only place a reference is created (two of them, one per flow) and dom_base_release the only place one is
   dropped. That equation is what makes the installed segment un-freeable during a sale — it is the running
   flow's own base, so any other chain that reaches it puts its count above 1 — and dom_install_chain asserts
   the consequence. A second way to take a reference would make it uncheckable. */
void dom_base_release(void *base);

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

/* DESTROY A DOCUMENT THIS ENGINE OWNS, and release the flow-level record that names it. What it adds over
   core/dom/node_interface.c's `dom_document_destroy` is exactly the thing a destroy cannot know: that a flow's
   delta owned this document, so its Document record goes back too. The identity map and the taint shadow are
   NOT this function's business — each node hands its own entries back as it dies, from the one point every node
   death converges on, so a document destroyed by any other caller is covered identically. */
void dom_cow_destroy_document(lxb_html_document_t *dom);

#endif
