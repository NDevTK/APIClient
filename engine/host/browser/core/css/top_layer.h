/* THE TOP LAYER — CSS Positioned Layout Level 4 §3 "Top Layer" and §3.3 "Top Layer Manipulation".
 *
 * "Documents have a top layer, an ordered set containing elements from the document." It is CSS's concept and
 * not HTML's — HTML §6.12 "The popover attribute", §4.11.4 "The dialog element" and Fullscreen all REACH for it
 * — so it is a component of its own, in the directory of the standard that defines it, rather than a static
 * inside whichever of those three landed first.
 *
 * THERE ARE TWO SETS AND NOT ONE, and the difference is the whole of §3.3's first two definitions: "an element
 * el is in the top layer if el is contained in its node document's top layer but not contained in its node
 * document's pending top layer removals". A single set cannot state that, and the note beside it says why the
 * distinction has to exist at all — a removal may be held open by an author transition, and a spec manipulating
 * the layer must not see the element leave before the rendering does.
 *
 * BOTH ARE JS ARRAYS ON THE DOCUMENT'S WRAPPER, under Symbols this file mints and never publishes. That is
 * CLAUDE.md's §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE rule applied to the one structure it is most about: an
 * ordered set with removal FROM THE MIDDLE, mutated by a flow, read back by a later flow, and carried across a
 * park. A malloc'd list captured as head/tail pointers reverts the POINTERS on a context switch and leaves the
 * nodes reachable from nothing — a leak the runtime's own gc_obj_list walk cannot name. An Array's mutations are
 * property writes the COW delta already captures, so two forked arms that show two different popovers each read
 * back their own layer, and a parked flow resumes with the one it had.
 *
 * THE SETS BELONG TO THE DOCUMENT AND NOT TO THE FLOW, which is not a contradiction of the paragraph above but
 * the reason it matters: §3.3 says "its node document's top layer", so the state is per-Document BASELINE state
 * that flows SHARE, and sharing is exactly the condition under which the delta must capture a write. A per-flow
 * structure would need no capture and would also be the wrong model — `document.querySelector('[popover]')` and
 * a sibling flow's `showPopover()` are two flows over one document. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_TOP_LAYER_H
#define ENGINE_HOST_BROWSER_CORE_CSS_TOP_LAYER_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT — the two slot keys identify the same two sets in every realm. */
void top_layer_declare(JSContext *ctx);
void top_layer_free(JSRuntime *rt);

/* §3.3's three manipulation algorithms, each given an ELEMENT (the sets are reached through its node document,
   which is what the standard writes: "let doc be el's node document"). */
void top_layer_add(JSContext *ctx, JSValueConst el);
void top_layer_request_removal(JSContext *ctx, JSValueConst el);
void top_layer_remove_immediately(JSContext *ctx, JSValueConst el);

/* "el is contained in doc's top layer" — the raw SET membership, which is the question §3.3's own three
   algorithms ask and the one HTML §6.12 The popover attribute's show popover step 6 asserts over.
   §3.3's derived concepts are NOT here and will arrive with their callers: "an element el IS IN the top layer"
   (contained, and not pending removal) has no consumer until HTML §6.12's Auto/Hint stack is built, and
   "rendered in the top layer" needs the `overlay` computed value this cascade has no property for. An exported
   predicate nothing asks is a reader with no writer wearing the other costume. */
bool top_layer_contains(JSContext *ctx, JSValueConst el);

/* ── §3's ORDER, READ ────────────────────────────────────────────────────────────────────────────────────────
 *
 * THE FIRST DERIVED CONCEPT TO ARRIVE, AND IT ARRIVES WITH ITS CALLER. That is the policy above obeyed rather
 * than an exception to it: Fullscreen §2 "Model"'s FULLSCREEN ELEMENT is "the topmost element in the document's
 * top layer whose fullscreen flag is set, if any, and null otherwise", so a walk exists to be exported.
 *
 * IT IS HERE AND NOT AT THE CALLER BECAUSE §3 SAYS SO IN ITS OWN WORDS: "The top layer (and the pending top
 * layer removals) should not be interacted with directly by specification algorithms." The ORDER is this
 * component's fact; a caller that indexed the Array itself would hold a second copy of it, and the copy that
 * drifts is the one whose owner never learns it moved.
 *
 * "TOPMOST" IS THE LAST MEMBER, WHICH §3 STATES AND NOTHING ELSE DECIDES: "Top layer elements are rendered in
 * the order they appear in the top layer; the last element in the top layer is rendered on top of everything
 * else." So the walk runs BACKWARDS and the first `pred` hit is the answer — the same direction
 * top_layer_process_removals runs, for a different reason.
 *
 * IT READS THE `top layer` AND NOT §3.3's "IS IN THE TOP LAYER", and the difference is observable rather than
 * pedantic: an element whose removal is pending is still CONTAINED in the set, so it is still eligible to be the
 * topmost match, and a walk that filtered `pending` would make a fullscreen element vanish one rendering update
 * early. Fullscreen §2's own reference is to §3's set. A caller that wants the filtered term asks for it when it
 * has one, which is the policy above again.
 *
 * IT ANSWERS WITH THE MEMBER AND NEVER WITH ITS RANK, which is the whole of why this is a walk and not an
 * indexed accessor. The set is mutated by algorithms a page reaches — `showPopover()`, `hidePopover()`,
 * `showModal()`, and Fullscreen's own fullscreen-an-element — so a POSITION recorded by a caller and replayed
 * after a suspension names whichever element has shifted into it, with every arm still in range and nothing to
 * say so (CLAUDE.md §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED). Returning the element itself is what
 * makes that impossible to write.
 *
 * `pred` DECIDES ONE MEMBER AND RUNS NO PAGE CODE. It is a C question about the element (Fullscreen's is one
 * own-slot read), asked inside a plain C activation with no flow base under it, so it may not run a page's
 * getter, fire an event or suspend. The walk asserts the half of that contract it can see — the set's length is
 * unchanged across the whole walk — which is what a predicate that reached a §3.3 algorithm would break.
 *
 * `document` is the Document WRAPPER, as top_layer_process_removals takes it. The answer is OWNED, and JS_NULL
 * when no member matches or when the document has never had a top layer at all. */
typedef bool (*TopLayerPredicate)(JSContext *ctx, JSValueConst el, void *opaque);
JSValue top_layer_topmost(JSContext *ctx, JSValueConst document, TopLayerPredicate pred, void *opaque);

/* §3.3's PROCESS TOP LAYER REMOVALS, given a Document. HTML §8.1.7's update the rendering step 23 is its one
   caller, and its note says so: "this is intended to be called during the 'Update the Rendering' step of HTML's
   rendering algorithm. It is not intended to be called by other algorithms." */
void top_layer_process_removals(JSContext *ctx, JSValueConst document);

#endif
