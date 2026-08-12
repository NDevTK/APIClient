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

#endif
