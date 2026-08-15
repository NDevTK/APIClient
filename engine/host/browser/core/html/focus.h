/* HTML §6.6 — THE FOCUS MODEL. See focus.c.
 *
 * The FOCUSED AREA OF THE DOCUMENT is real per-document, per-flow state, and everything else in §6.6 is stated
 * over it: `document.activeElement`, `documentOrShadowRoot.activeElement`, `document.hasFocus()`, the focusing,
 * unfocusing and focus update steps, and the `focus`/`blur` events those steps fire. It was ABSENT, behind a
 * comment claiming a headless run has no focus and that the spec defines no scriptable result — which is false
 * twice, since the focused area IS scriptable state and the events it fires are the page's own code. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FOCUS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FOCUS_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* THE AGENT'S HALF: the realm slot §6.6.2's focused area lives in, and the three declared members. Reached from
   document_init, beside §6.6's visibility state and for the same reason it is paired there rather than copied
   into each host's init list — the focused area is a DOCUMENT's state and document.c is the one component every
   host that has a Document goes through. */
void focus_init(JSContext *ctx);

/* §6.6.6's `activeElement` (DocumentOrShadowRoot) and `hasFocus()` (Document), plus THIS REALM'S initial
   focused area — the record is built here for the reason document.c builds its readiness here: a record made on
   first touch would be made inside whichever flow happened to read first, and would then be that flow's
   document rather than the baseline every flow forks from. */
void focus_install_document_members(JSContext *ctx, JSValueConst proto);
/* The same mixin's ONE member on ShadowRoot, whose getter RETARGETS against the receiver — which is why the
   member is one implementation over two interfaces and not two getters. */
void focus_install_shadow_root_members(JSContext *ctx, JSValueConst proto);
/* §6.6.6's `focus(options)` and `blur()` — HTMLOrSVGOrMathMLElement's, installed on HTMLElement.prototype. */
void focus_install_html_members(JSContext *ctx, JSValueConst proto);
/* §6.6.6's `Window.focus()`. `Window.blur()` is NOT here: §6.6.6 states its method steps as "do nothing", so
   it is the spec's own no-effect and belongs with the Window's other members. */
void focus_install_window_members(JSContext *ctx, JSValueConst global);

/* ---- HTML §8.1.7.3 update the rendering step 17 — the FOCUS FIXUP RULE --------------------------------------
 *
 * "For each doc of docs, if the focused area of doc is not a focusable area, then run the focusing steps for
 * doc's viewport, and set doc's relevant global object's navigation API's focus changed during ongoing
 * navigation to false." Both halves the rendering algorithm needs are §6.6's, so both are answered HERE and the
 * step reads as the sentence it implements — the alternative is a second copy of "what a focusable area is" in
 * rendering.c, which is the one-fact-two-answers defect.
 *
 * THE CONDITION. §6.6.2 designates ONE focusable area of each Document as its focused area, and this step
 * exists because the tree can stop making that designation true after the fact: `hidden` added to an ancestor,
 * an `input` disabled, the element disconnected. Asked of the realm whose ACTIVE DOCUMENT doc is. The VIEWPORT
 * is a focusable area whenever the Document has a non-null browsing context — which is exactly what holding a
 * §6.6.2 record means — so only the element designation can ever answer false.
 *
 * THE ACTION IS A REQUEST, because §6.6.4's focusing steps fire `blur`, `focusout`, `focus` and `focusin` at
 * the page's own listeners: the calling machine parks on it and resumes with the algorithm finished, exactly as
 * it parks on a fire. It reaches the ONE focus machine through its own entry point (focus.c's magic), so
 * nothing of §6.6.4 is written twice. `phase` and `cb` belong to the CALLING machine — it visits them, so a
 * fork copies them and a suspension inside a `blur` listener resumes in the same stage — and `cb` is passed
 * through STEP_CB so its capacity travels with it.
 *   JS_STEP_CALL = return it, 0 = the focusing steps have finished. */
/* step_call_run's operand shape is [this, func, args…] and the focusing steps take no arguments. A caller whose
   one buffer serves several requests declares it as the WIDEST of them (EventFireCb) rather than counting; this
   is the floor that the request asserts against on both legs. */
#define FOCUS_VIEWPORT_CB_SLOTS (2 + 0)
bool focus_focused_area_is_focusable(JSContext *docctx);
int  focus_viewport_run(JSContext *docctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValue in,
                        JSValue **out_cb, int *out_argc);

#endif
