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

/* THE ORDER'S OWN INPUTS, COMPOSED ONCE — solver/flow.h's WfqCensus rendered as one JSON object (caller
 * frees), which `result_json` embeds as the document's `_wfq` and which a host with a line-oriented output
 * prints verbatim. ONE composer, because the alternative is what it replaced: a struct whose rows are added by
 * hand in flow.h and serialized by a printf in a THIRD file that is also maintained by hand, which is how
 * `svc_min` came to be computed on every census and printed by nobody, and how `families` came to be printed
 * and read by nobody.
 *
 * IT WAS EMITTED ONLY FROM A HOST LOOP THE PRODUCTION ABI NEVER ENTERS, WHICH IS WHY IT MOVED HERE. The
 * scheduler's ordering is the one thing §scheduler makes a claim about at BOTH levels, and the only place it
 * was ever written was `run_scheduler` — the smoke driver's loop over `engine_sched_step`, which `qjs_step`
 * does not call and which no extension has ever run. So every ordering number this project has quoted is a
 * reading of one fixture, and the row that would have shown a rank frozen at a constant was emitted by
 * nothing a person ever runs. The result document is the surface that already crosses the ABI, is already
 * asserted field-for-field by the trusted zone, and is already published DURING a run (main.c's
 * qjs_emit_partial, on the host's own cadence) as well as at its end — so putting the census on it is what
 * makes the ordering observable where it matters, and §Testing's "read the result DOCUMENT rather than the
 * log" is the same sentence one layer up.
 *
 * TWO STATES, AND THE SECOND IS A POSITIVE STATEMENT RATHER THAN A ROW OF ZEROES. A census is a reading of an
 * INSTANT, and the instant `qjs_result` composes at is the one where the frontier has just drained or been
 * parked — so `members` is 0 and there is no order to report. Emitting `valMin: 0, wTop: 0, …` there would
 * fabricate readings of an order that does not exist, which is §@H's own rule ("never invent") performed on
 * the engine's own diagnostics, and a reader taking the last document would then be told "no term orders this
 * frontier" about a run that drained. So an empty frontier emits `{"members":0}` and NOTHING ELSE: the
 * ABSENCE of the term rows IS the statement that there was no order, exactly as §Architecture requires a
 * legitimately-omitted value to be read. The three facts a consumer must keep apart are then distinguishable:
 * no `_wfq` at all is a BROKEN CONTRACT, `{"members":0}` is an EMPTY FRONTIER, and a full object is a
 * READING — where before, all three would have been the same zeroes. */
char *result_wfq_json(void);

/* ---- THE THREE CENSUSES THAT RODE NOTHING -----------------------------------------------------------------
 *
 * `result_wfq_json` moved here because the ordering was written only by `run_scheduler`. THESE THREE HAVE THE
 * IDENTICAL DEFECT AND IT IS THE LARGER HALF OF IT: `@COLD`, `@HEAP` and `@SWAP` are also printed only from
 * that loop, which `engine_run` alone enters and which `qjs_step` does not, so between them SIXTY-NINE computed
 * numbers reached a host the extension never runs and NOT ONE of them has ever been readable off a production
 * run. §Testing's rule is the whole of the argument — "measure what the shipped path writes, not what a
 * harness prints" — and its consequence is not that the numbers were wrong: it is that the subsystems they
 * measure have never been measured where they actually do their work. The pager pages under RAM pressure in
 * the EXTENSION; the realm ceiling is reached on REAL pages; a delta chain accumulates over a REAL frontier.
 *
 * SO THEY RIDE THE DOCUMENT, and the document is already a TIME SERIES on the shipped path: `main.c`'s
 * `qjs_emit_partial` composes a fresh one on the host's own cadence for the whole of a long analysis. A census
 * is a READING OF AN INSTANT rather than a total, and the objection that one end-of-run record cannot express
 * a per-step series is FALSE OF THIS DOCUMENT for exactly that reason.
 *
 * ONE COMPOSER, TWO EMISSION SITES, AND THE BYTES ARE THE SAME BYTES. `run_scheduler` still prints its lines —
 * a host whose output IS a stream of lines has to print a census when it is taken or it is not in the output at
 * all — but it prints what these produced. Neither side hand-serializes a struct any more, which is what
 * `result_wfq_json`'s own history says goes wrong: a struct maintained by hand in one file and serialized by a
 * printf in a third is two hand-kept lists that drift in BOTH directions at once.
 *
 * WHY THREE AND NOT ONE. Each answers a different subsystem's question and a reader compares WITHIN one, never
 * across: `_swap` is what a CONTEXT SWITCH costs and what the two COW chains are still holding; `_cold` is what
 * the FRONTIER is made of and what its parked snapshots weigh; `_heap` is what the RUNTIME and the C allocator
 * under it hold. Folding them into one object would put a switch count beside a byte count beside a realm
 * count and invite exactly the comparison none of them supports.
 *
 * AND UNLIKE `_wfq` THERE IS NO EMPTY SHAPE. The WFQ's rows are readings of a FRONTIER, which can legitimately
 * have no members — so its absence of rows is a positive statement. These three read the allocator, the
 * runtime and this instance's own totals, all of which exist at every instant a document is composed at, so
 * every row is always present and a missing one is a broken contract with no second reading. Caller frees. */
char *result_cold_json(void);
char *result_heap_json(JSContext *ctx);
char *result_swap_json(void);

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
