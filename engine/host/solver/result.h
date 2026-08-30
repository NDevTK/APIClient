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
#include <stddef.h>

#include "quickjs.h"

/* The whole document as a malloc'd JSON string (caller frees):
 *   { "fetchCallSites":[…], "securitySinks":[…], "_switches":N }
 * `_switches` is the scheduler's context-switch count — the host's WFQ reads it as the observable that the ONE
 * BFS actually interleaves rather than running its flows FIFO. */
char *result_json(JSContext *ctx);

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
/* THE SAME DESCRIPTION, INTO THE CALLER'S BUFFER, because a thrown value has to be readable somewhere other
   than the findings document. An assert that names a failure and DISCARDS the exception describing it names a
   problem nobody can act on: `flow_step: a page <script> did not COMPILE` was measured on five of eleven real
   production bundles and said nothing whatever about WHICH construct the parser refused, while the SyntaxError
   carrying the construct and its position was freed one line below. Runs no page code, for the reason above. */
void result_error_text(JSContext *ctx, JSValueConst err, char *out, size_t outsz);

/* AND WHO REPORTS ONE AS IT HAPPENS, which is the HOST's question and not this file's — ASKED OF EVERY HOST,
   because the two answers are not distinguishable from the absence of either. A host whose output is a
   DOCUMENT reads `pageErrors` out of it at the end; a host whose output IS a stream of lines has to print it
   when it occurs or it is not in the output at all — the wpt runner used to catch every program's exception
   itself, at the `while (JS_FlowResume)` that ran it, and with the programs on the scheduler there is no such
   place left.
   REGISTERING NONE USED TO BE THE FIRST ANSWER SPELLED AS AN ABSENCE, AND FOR THE HOST IT MATTERED MOST IN IT
   WAS FALSE. A document is only an answer if the document is PUBLISHED, and a host that renders one at the end
   of a run publishes nothing on a run that does not reach the end. The smoke fixture is exactly that host: its
   document is rendered after the scheduler returns, and its frontier drains only once every probe row is 1 —
   so on precisely the runs where a row is 0, `pageErrors` is composed and freed unread. Measured: four smoke
   logs, not one of them carrying an `@RESULT` line, while an uncaught throw ended a <script> more than a
   thousand statements before the @S sinks that script contains. The run reported a hundred zeroes and nothing
   named the throw — which is the defect this whole surface exists to end, performed on itself.
   SO THE CHOICE IS DECLARED AND NEVER DEFAULTED. A NULL hook meaning "document" made the host that had thought
   about it and the host that had not produce the identical call, which is a producer's field a consumer
   defaults; both forms are positive statements now, and result_page_error asserts one of them was made before
   it records anything. The hook is called once per DISTINCT message, since that is what the set below holds. */
void result_set_page_error_hook(void (*fn)(const char *msg));
/* The other half of that declaration: this host PUBLISHES result_json unconditionally and reads `pageErrors`
   out of it. Say it where the host states its other edges, beside WHO answers the network and WHO evaluates a
   string handler — a page error's reader is an edge of exactly that kind. */
void result_page_errors_ride_the_document(void);

#endif
