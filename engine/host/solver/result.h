/* THE RESULT DOCUMENT — one structure, built in C, read once by the host.
 *
 * The host is a BRIDGE and not a layer: it does ONE JSON.parse of one `@RESULT <json>` line and relays the
 * object. It does not stitch surfaces together, because stitching is structure, and structure here is the
 * engine's — the same reason identity and dedup are. Two lines (`@RESULT` endpoints, `@SEC` sinks) would make
 * the host assemble a document from parts, and a host that assembles is a host that can assemble wrongly:
 * an XSS-only page carries verified @S PoCs and no endpoints, and getting that case right is a property of
 * one document, not of two lines that happen to arrive together.
 *
 * Only what the ENGINE knows is emitted. The host fills its own empties (source maps, proto field maps) and
 * defaults anything absent, so a field this engine cannot yet answer is LEFT OUT rather than emitted as a
 * zero that reads like an answer.
 */
#ifndef ENGINE_HOST_SOLVER_RESULT_H
#define ENGINE_HOST_SOLVER_RESULT_H
#include "quickjs.h"

/* The whole document as a malloc'd JSON string (caller frees):
 *   { "fetchCallSites":[…], "securitySinks":[…], "_switches":N }
 * `_switches` is the scheduler's context-switch count — the host's WFQ reads it as the observable that the ONE
 * BFS actually interleaves rather than running its flows FIFO. */
char *result_json(void);

/* AN UNCAUGHT ERROR FROM ONE OF THE PAGE'S OWN SCRIPTS. A page's throw ending its script is intentional — it is
   the forcing function that names an unbuilt capability — but the name was invisible: the flow simply stopped
   and the document reported the surface it had reached, with nothing to say a script had died. Recording it
   makes the capability the page needed READABLE, which is the difference between "this page yields little" and
   "this page needs Element.matches". Deduped; the document carries them as `pageErrors`. */
void result_page_error(const char *msg);
/* The same, from the thrown VALUE. It runs NO page code: `toString` on an Error is the page's (and in this
   engine a step builtin the interpreter must dispatch), so this reads the own `name`/`message` slots and uses
   them only when they are already strings. A diagnostic that runs the page's code to describe the page's crash
   is a second crash. */
void result_page_error_value(JSContext *ctx, JSValueConst err);

#endif
