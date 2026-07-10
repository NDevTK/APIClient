#ifndef ENGINE_HOST_HTML_SCRIPT_ELEMENT_H
#define ENGINE_HOST_HTML_SCRIPT_ELEMENT_H
#include "quickjs.h"
#include <lexbor/dom/dom.h>
/* HTMLScriptElement / ScriptLoader (Blink core/html): when a <script src> is inserted into the live DOM, Blink
 * fetches + runs it. Here that is LAZY-CHUNK discovery — the src (possibly a JS-computed URL held in the
 * attribute taint shadow) is queued for fetch so the chunk's endpoints + code are learned. A pure browser
 * component: it FEEDS the scheduler (chunk queue) and holds no control flow of its own. */
void script_maybe_load(JSContext *ctx, lxb_dom_element_t *el);
#endif
