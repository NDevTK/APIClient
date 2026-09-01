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

/* §3.3's PROCESS TOP LAYER REMOVALS, given a Document. HTML §8.1.7's update the rendering step 23 is its one
   caller, and its note says so: "this is intended to be called during the 'Update the Rendering' step of HTML's
   rendering algorithm. It is not intended to be called by other algorithms." */
void top_layer_process_removals(JSContext *ctx, JSValueConst document);

#endif
