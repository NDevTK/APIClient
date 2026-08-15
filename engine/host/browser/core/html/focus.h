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

/* ---- HTML §6.6.7's three calls into §6.6 --------------------------------------------------------------------
 *
 * THE AUTOFOCUS ATTRIBUTE is core/html/autofocus.c: it owns §6.6.7's own state (a Document's autofocus
 * candidates list and its autofocus processed flag) and the two algorithms stated over it — the insertion steps
 * that fill the list and the flush that drains it. But every DECISION those algorithms make is one of §6.6's,
 * named by the standard at the step that makes it, so each is answered HERE and the step reads as the sentence
 * it implements. The alternative is a second copy of "what a focusable area is" in autofocus.c — the same
 * one-fact-two-answers defect the rendering step above avoids for the same reason. */

/* §6.6.6's ALLOW FOCUS STEPS given a Document, which §6.6.7's insertion steps run as their step 5. */
bool focus_allow_focus_steps(JSContext *docctx);

/* §6.6.7 flush step 4's first disjunct, negated — "topDocument's focused area is topDocument itself". §6.6.2's
   focused area is either an element or the VIEWPORT, whose DOM anchor IS the Document, so the spec's sentence
   is exactly "the focused area is the viewport" here (focus.c's header states why this engine designates the
   viewport where the standard's prose leaves the initial designation to the user agent). */
bool focus_focused_area_is_viewport(JSContext *docctx);

/* §6.6.7 flush steps 5.9-5.10's verdict: target is the candidate, and if that is not a focusable area then the
   result of GETTING THE FOCUSABLE AREA for it — is that target non-null? A question rather than a request,
   because §6.6.4's delegate search walks content attributes and shadow roots and runs no page code. */
bool focus_focusable_area_exists(JSContext *docctx, JSValueConst el);

/* §6.6.7 flush step 5.11.3's "run the focusing steps for target", as a request the calling machine parks on —
   the same two-leg shape and the same reason as focus_viewport_run. `docctx` must be the realm whose ACTIVE
   DOCUMENT is the candidate's node document (§6.6.7 step 5.4 admits candidates from any document under
   topDocument's top-level traversable, so that is routinely a CHILD realm), which focus.c asserts at the door.
   The ELEMENT is the algorithm's own focus target: §6.6.4 step 1 re-derives the focusable area from it, which
   is the identical answer focus_focusable_area_exists just gave, since nothing between them runs page code. */
#define FOCUS_ELEMENT_CB_SLOTS (2 + 1)
int  focus_element_run(JSContext *docctx, JSValueConst el, uint8_t *phase, JSValue *cb, int cb_cap, JSValue in,
                       JSValue **out_cb, int *out_argc);

#endif
