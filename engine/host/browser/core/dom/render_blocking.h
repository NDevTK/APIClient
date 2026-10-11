/* HTML §3.1.6 Render-blocking mechanism: each Document's render-blocking element set, and whether the Document
 * is render-blocked (Blink's RenderBlockingResourceManager). §8.1.7.3 update the rendering step 3 removes a
 * render-blocked document from the rendering.
 *
 * The set is a per-realm record of element wrappers, so every write rides the running flow's COW delta.
 *
 * The parse position. §3.1.6's "the body element of document is null" is a question about how far the parser
 * has got. This engine parses a loaded document whole before any of its scripts run, so the tree cannot answer
 * it; the answer is asked at the parse position instead: the script element the parser stands at in the running
 * flow (solver/engine.h's engine_document_parse_position), or, for an act the parser performs itself, the element
 * it is acting on. NULL is a parse that has passed every script holding a position, where the tree answers. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_RENDER_BLOCKING_H
#define ENGINE_HOST_BROWSER_CORE_DOM_RENDER_BLOCKING_H

#include <stdbool.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void render_blocking_init(JSContext *ctx);      /* the per-realm slot; declared from document_init */
void render_blocking_install(JSContext *ctx);   /* this realm's empty set; from document_install_proto */
void render_blocking_free(void);                /* from document_agent_free */

/* Where the parser of `ctx`'s document stands in the running flow (see above). Requires a running flow. */
const lxb_dom_node_t *render_blocking_parse_position(JSContext *ctx);

/* §3.1.6 "allows adding render-blocking elements" for `doc`, with the parser at `at`. */
bool render_blocking_allows_adding(const lxb_dom_node_t *doc, const lxb_dom_node_t *at);

/* §3.1.6 "block rendering on an element el", with the parser at `at`, and "unblock rendering on an element el".
   Both act on el's node document; an element of a document no realm holds has no set and nothing happens. */
void render_blocking_block(lxb_dom_element_t *el, const lxb_dom_node_t *at);
void render_blocking_unblock(lxb_dom_element_t *el);

/* §3.1.6 "render-blocked", for `ctx`'s active document. Requires a running flow. */
bool render_blocking_document_render_blocked(JSContext *ctx);

#endif
