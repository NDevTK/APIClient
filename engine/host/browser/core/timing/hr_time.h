/* HIGH RESOLUTION TIME Level 3 §4 — THE TIME ORIGIN, and the one operation every page-visible timestamp in
 * this engine is a value of.
 *
 * WHY THIS IS A COMPONENT AND NOT A SUBTRACTION AT EACH SITE. Three algorithms in this build each answered
 * "what number does the page see for this moment" for themselves, out of the raw virtual clock:
 *   - DOM §2.5's inner event creation steps initialize `timeStamp` to "the RELATIVE high resolution coarse
 *     time given time and event's relevant global object", and the constructor to "the result of calling
 *     CURRENT high resolution time with this's relevant global object" — core/events/event.c wrote
 *     `event_loop_now(ctx)`.
 *   - HTML §6.4.1's three activation questions are all "the CURRENT high resolution time given W" compared
 *     against a stored timestamp — core/html/user_activation.c wrote its own `ua_now` over the same clock,
 *     and its comment said so.
 *   - HTML §8.1.7.3 step 14 passes "the RELATIVE high resolution time given frameTimestamp and doc's relevant
 *     global object" to every animation frame callback — core/rendering/rendering.c passed frameTimestamp
 *     itself, under a comment claiming it was already the relative time.
 * That is CLAUDE.md's one-fact-answered-from-many-places in its plainest form, and the third site shows what
 * it costs: the answers are only equal while every environment's TIME ORIGIN is zero, and the moment a second
 * document exists they are not.
 *
 * THE TIME ORIGIN IS AN ENVIRONMENT SETTINGS OBJECT FIELD, which is why it lives in core/realm.h's per-realm
 * store beside HTML §8.1.3.1's top-level creation URL rather than on a document or in a static. §4 says the
 * field holds "a moment early in the initialization of a relevant environment settings object", and realm.h
 * already states that this engine creates that environment WITH the realm — so the one call every realm goes
 * through is literally the moment the standard describes, and the agent's first realm gets one by the same
 * mechanism a child navigable's does.
 *
 * IT HAS NO WRITER AFTER CREATION, so it is a plain per-realm value and NOT a COW-captured heap record. That
 * is the test core/frame/viewport.h states from the other side: §13.1's latch has a writer (every run of the
 * resize steps), so it is per-flow state in the delta; a time origin is written once, by the install that
 * builds the realm, and read forever. Two flows exploring one document therefore agree about it, which is
 * correct — they are the same environment.
 *
 * WHY IT IS NOT CONCOLIC. viewport.h's test is whether the model PICKED a point out of a range the environment
 * leaves free (a source, carrying its computed answer as an example) or DERIVED the only value the rest of the
 * model permits (concrete). A time origin is the second: it is the moment on the ONE virtual clock at which
 * this realm was built, and the clock is a function of which tasks this flow has run. Forking it would run a
 * page in a world where its own document was created at a moment the event loop never reached.
 *
 * COARSENING IS MODELLED AND NOT SKIPPED, and it is deliberately NOT jittered. §4's coarsen time floors the
 * moment onto a resolution grid and step 3 permits an implementation to JITTER it as well. This engine takes
 * the floor and declines the jitter, and that is a design constraint of the whole project rather than a
 * simplification: §Testing's solver differential is the only oracle the SOLVER's semantics have, and its whole
 * content is that one build must agree with ITSELF about a document across several schedules. A jittered clock
 * makes every timestamp a source of disagreement that no scheduling bug is needed to produce, so the gate would
 * report noise and stop meaning anything. Determinism here is not "no randomness in this file" — it is that a
 * coarsened moment is a pure function of the virtual clock and the resolution, and the resolution is itself a
 * pure function of this environment's cross-origin isolated capability, which has no writer.
 *
 * WHICH RESOLUTION IS THE ENVIRONMENT'S ANSWER, NOT A CONSTANT. §4 makes it 100 microseconds, or 5 for an
 * environment with the CROSS-ORIGIN ISOLATED CAPABILITY — HTML §7.2.2's environment settings object field,
 * which core/frame/agent_cluster.h computes from this agent cluster's §7.1.4 isolation mode. `hr_time_coarsen`
 * asks that component. It used to assert the capability's ABSENCE instead (`realm_awaits(ctx,
 * "crossOriginIsolated", ...)`), which was correct for exactly as long as no realm could answer the question
 * and became a crash on the first coarsen of every run the moment `window.crossOriginIsolated` landed. The
 * capability EXISTS now and answers false for every environment this build makes, because no COOP/COEP response
 * header reaches a policy container; what is still unbuilt is named where it is missing, by the DFAIL on
 * §7.2.2's permissions-policy conjunct in agent_cluster.c, which is unreachable until the mode can be
 * `concrete`. THE ASSERTION MOVED TO THE REAL ABSENCE; it was not deleted. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_HR_TIME_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_HR_TIME_H

#include "quickjs.h"

/* Declared ONCE PER AGENT — the per-realm slot the time origin lives in, and the per-realm install that stamps
   it. Released through the platform list's third column, so no host has a line to remember. */
void hr_time_init(JSContext *ctx);
void hr_time_free(void);

/* §4's TIME ORIGIN of THIS realm's environment settings object, as a moment on the one virtual clock. Exported
   because the day this engine has a `performance` object, `get time origin timestamp` is its second reader. */
double hr_time_origin(JSContext *ctx);

/* §4's COARSEN TIME, given an unsafe moment on the monotonic clock. §4's second argument is the environment's
   cross-origin isolated capability, and `ctx` IS that environment — a caller passes the realm whose settings
   object the moment is FOR, never the realm the algorithm happens to be driven from. Answers a moment, never a
   duration — the spec coarsens the ABSOLUTE moment and subtracts the origin afterwards, and doing it the other
   way round would round a duration whose two ends are on the grid. */
double hr_time_coarsen(JSContext *ctx, double unsafe_moment);

/* §4's RELATIVE HIGH RESOLUTION TIME given an unsafe moment and a global object — coarsen the moment with this
   environment's capability, then take the duration from this environment's time origin to it. This is the
   operation HTML §8.1.7.3 steps 11, 14, 19 and 20 name in their own argument lists, and DOM §2.5 names for
   `timeStamp`; `ctx` IS the relevant global object those steps mean, so a per-document step must pass the
   DOCUMENT's realm and never the realm the algorithm happens to be driven from. */
double hr_time_relative(JSContext *ctx, double unsafe_moment);

/* §4's CURRENT HIGH RESOLUTION TIME given a global object — the relative high resolution time of the unsafe
   shared current time, which in this engine is the event loop's virtual clock (core/timing/event_loop.h). */
double hr_time_current(JSContext *ctx);

#endif
