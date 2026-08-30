/* ANIMATION FRAMES — HTML §8.12 Animation frames "Animation frames", the map every Window has and the two members that fill it.
 *
 * §8.12 Animation frames is not the rendering loop; it is the DATA the rendering loop's step 14 consumes. Keeping the two apart
 * is the point of this file existing beside rendering.c: `requestAnimationFrame` is a Window member with an
 * IDL of its own, and "run the animation frame callbacks" is one algorithm over one map — while "update the
 * rendering" is twenty-three steps over a list of documents, of which this is step 14.
 *
 * THE MAP IS A HEAP OBJECT AND THAT IS LOAD-BEARING. A registered callback is per-FLOW state: one arm of a
 * fork may register an animation callback its sibling never registered, and a parked flow must resume owed
 * exactly the callbacks it was owed. A C-side list would be one map answering for every flow, and the delta
 * has nothing to capture. A baseline object built at realm install is captured by the heap COW for free —
 * every registration is a property write — and it parks and resumes with the flow like everything else.
 *
 * §8.12 Animation frames's SNAPSHOT-THEN-RECHECK IS THE WHOLE ALGORITHM AND IT IS OBSERVABLE. Step 2 takes the KEYS, step 3
 * re-checks each one against the live map before taking it. That is what makes a `requestAnimationFrame`
 * issued from inside a callback run on the NEXT frame (its key is not in the snapshot) and a
 * `cancelAnimationFrame` issued from inside one take effect on THIS frame (its key is in the snapshot and no
 * longer in the map). A machine that re-read the map instead would run a self-rescheduling animation callback
 * forever inside one frame, which is a page that hangs rather than a page that animates. */
#ifndef ENGINE_HOST_BROWSER_CORE_RENDERING_ANIMATION_FRAME_H
#define ENGINE_HOST_BROWSER_CORE_RENDERING_ANIMATION_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* THE AGENT'S HALF — §8.12 Animation frames's two members declared once, and the per-realm slot the map lives in. */
void animation_frame_init(JSContext *ctx);
/* THE REALM'S HALF, and it is TWO calls because they answer to two different owners. The MAP is a per-realm
   intrinsic (§3.7): every realm gets one through the one declared list realm.h holds, including the agent's
   first, so a realm cannot come into existence without it. The MEMBERS go on the Window, which is the host's
   per-document install. */
void animation_frame_install_map(JSContext *ctx);
void animation_frame_install(JSContext *ctx, JSValueConst global);
/* THE AGENT'S HALF, UNDONE — core/platform.c's release column, which takes the RUNTIME because that is what an
   agent is. */
void animation_frame_free(JSRuntime *rt);

/* §8.1.7.3 "update the rendering" step 4's test, for THIS document: is its map of animation frame callbacks
   non-empty? The rendering loop asks it twice — once to decide whether a navigable might have a rendering
   opportunity at all, and once as the step-4 filter — because those are two different moments and the map is
   the page's to change between them. */
bool animation_frame_pending(JSContext *ctx);

/* §8.12 Animation frames step 2 — "let callbackHandles be the result of getting the keys of callbacks", as the COUNT of entries
   present when the run begins. Handles are appended in issue order and never move, so the prefix [0, n) IS
   the key snapshot: an entry appended by a callback lands past it and is not visited. */
uint32_t animation_frame_snapshot(JSContext *ctx);

/* §8.12 Animation frames step 3 — "if handle exists in callbacks: let callback be callbacks[handle]; remove callbacks[handle]".
   Answers the callback (OWNED) and REMOVES it, or JS_UNDEFINED when the entry no longer exists because a
   `cancelAnimationFrame` in an earlier callback of this same frame took it out. */
JSValue animation_frame_take(JSContext *ctx, uint32_t i);

/* The end of §8.12 Animation frames's walk: every entry of the snapshot has been taken or was already gone, so the prefix is
   dead and the entries a callback appended become the next frame's. Not a compaction for tidiness — an
   animation that reschedules itself every frame runs forever by design, and a map that only ever grew would
   be an unbounded allocation with nothing to say so. */
void animation_frame_run_end(JSContext *ctx, uint32_t consumed);

#endif
