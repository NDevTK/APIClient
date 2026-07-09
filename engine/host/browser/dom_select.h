/* CSS selector engine over the live Lexbor DOM — querySelector/All, getElementById/ClassName, matches/closest.
 * A cohesive component: fresh selector+parser objects PER CALL (reuse corrupts the next query), searching the
 * whole document or a subtree `root`. Its only ties to the rest are the document (g_dom) and the element→JS
 * wrapper (el_wrap), both defined in main.c. No scheduler/flow state. */
#ifndef ENGINE_HOST_DOM_SELECT_H
#define ENGINE_HOST_DOM_SELECT_H

#include <stddef.h>
#include <lexbor/html/html.h>
#include "quickjs.h"

/* First element matching `sel` under `root` (NULL root = whole document). Owned by g_dom. */
lxb_dom_element_t *dom_select_first(lxb_dom_node_t *root, const char *sel, size_t len);

/* All matches as a JS array of wrapped elements (so a for-of over the result iterates real elements). */
JSValue dom_select_all(JSContext *ctx, lxb_dom_node_t *root, const char *sel, size_t len);

/* Does THIS element match `sel`? (element.matches / the per-ancestor test for closest). */
int dom_node_matches(lxb_dom_element_t *el, const char *sel, size_t len);

#endif
