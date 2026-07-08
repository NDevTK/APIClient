/* document.write() — an @S sink AND a script loader.
 *
 * The written HTML is attacker-influenceable, so it's an XSS sink (solve_add). But we ARE the browser: a
 * document.write of a `<script>` must RUN — inline script bodies are eval'd top-level, and a written
 * `<script src>` is a chunk LOAD (fetched + forced-executed), so the endpoints/sinks behind lazily-written
 * code are explored, not merely string-extracted. */
#ifndef ENGINE_HOST_DOCWRITE_H
#define ENGINE_HOST_DOCWRITE_H

#include "quickjs.h"

JSValue js_doc_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

#endif
