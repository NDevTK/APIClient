/* HIGH RESOLUTION TIME Level 3 §4 — THE TIME ORIGIN, and the one operation every page-visible timestamp in
 * this engine is a value of.
 *
 * WHY THIS IS A COMPONENT AND NOT A SUBTRACTION AT EACH SITE. Three algorithms in this build each answered
 * "what number does the page see for this moment" for themselves, out of the raw virtual clock:
 *   - DOM §2.5 Constructing events' inner event creation steps step 3 initializes `timeStamp` to "the RELATIVE
 *     high resolution coarse time given time and event's relevant global object", and DOM §4.5 Interface
 *     Document's `createEvent()` step 7 to "the result of calling CURRENT high resolution time with this's
 *     relevant global object" — TWO DIFFERENT §4 OPERATIONS, in two different sections, and this file used to
 *     put both sentences in §2.5. core/events/event.c wrote `event_loop_now(ctx)` for them.
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
 * WHY IT IS NOT A SOURCE. viewport.h's test is whether the model PICKED a point out of a range the environment
 * leaves free (a source, carrying its computed answer as an example) or DERIVED the only value the rest of the
 * model permits. A time origin is the second: it is the moment on the ONE virtual clock at which this realm
 * was built, and the clock is a function of which tasks this flow has run. Forking it would run a page in a
 * world where its own document was created at a moment the event loop never reached.
 *
 * WHICH IS NOT THE SAME AS SAYING IT IS A NUMBER, and this used to say it was. The clock itself is a MOMENT
 * and a moment can be unknown external input (core/timing/event_loop.h: §8.7's `timeout` reaches the map of
 * active timers, so firing that timer moves the clock to a moment nothing computed). A realm created after
 * such a task has fired therefore has a time origin DERIVED from an unknown, which is still a derivation and
 * still not a source — nothing picked it, and it carries the arithmetic run on whatever example the clock
 * had. Every operation below answers a moment or a duration as the VALUE it is, for the same reason.
 *
 * AND THE COARSENING IS A DERIVATION RATHER THAN A SECOND STORED FACT. §4's coarsen time is an operation over
 * the moment, so it is COMPUTED at every read from the moment itself — never floored once and kept beside it.
 * Two stored copies of one fact is the defect this tree keeps finding, and here the second copy would go
 * stale the instant the clock moved. Over an unknown the operation is the SAME operation: the result carries
 * the moment's provenance, names §4's algorithm as what produced it, and takes as its example the real floor
 * run on the moment's own example. It is one hop and not a recorded expression — §Re-execution forbids the
 * second — and it is why `hr_time_coarsen` may never be asked to answer a `double`.
 *
 * COARSENING IS MODELLED AND NOT SKIPPED, and it is deliberately NOT jittered. §4's coarsen time floors the
 * moment onto a resolution grid and step 3 permits an implementation to JITTER it as well. This engine takes
 * the floor and declines the jitter, and that is a design constraint of the whole project rather than a
 * simplification: §Testing's solver differential is the only oracle the SOLVER's semantics have, and its whole
 * content is that one build must agree with ITSELF about a document across several schedules. A jittered clock
 * makes every timestamp a source of disagreement that no scheduling bug is needed to produce, so the gate would
 * report noise and stop meaning anything. Determinism here is not "no randomness in this file" — it is that a
 * coarsened moment is a pure function of the virtual clock and the resolution, and the resolution is itself a
 * pure function of this environment's cross-origin isolated capability, which nothing in a run can change.
 *
 * WHICH RESOLUTION IS THE ENVIRONMENT'S ANSWER, NOT A CONSTANT. §4 makes it 100 microseconds, or 5 for an
 * environment with the CROSS-ORIGIN ISOLATED CAPABILITY — HTML §7.2.2.6 "Script settings for Window objects"'
 * environment settings object field, which core/frame/agent_cluster.h computes from BOTH of its conjuncts: the
 * browsing context group's §7.3.2.3 cross-origin isolation mode, and HTML §4.8.5's allowed-to-use over the
 * Document's Permissions Policy §9.5 policy. `hr_time_coarsen` asks that component and nothing else.
 * IT USED TO ASSERT THE CAPABILITY'S ABSENCE instead (`realm_awaits(ctx, "crossOriginIsolated", ...)`), which
 * was correct for exactly as long as no realm could answer the question and became a crash on the first coarsen
 * of every run the moment `window.crossOriginIsolated` landed. BOTH CONJUNCTS ARE BUILT NOW AND THE 5µs ARM IS
 * REACHABLE: a page served `Cross-Origin-Opener-Policy: same-origin` beside a `Cross-Origin-Embedder-Policy`
 * compatible with cross-origin isolation gets `concrete` from §7.3.2.3's create, and a top-level Document in it
 * is allowed to use the `cross-origin-isolated` feature — so this file's grid is 5µs for such a page and 100µs
 * for its cross-origin frames, which is the whole reason the resolution is asked per environment. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_HR_TIME_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_HR_TIME_H

#include "quickjs.h"

/* Declared ONCE PER AGENT — the per-realm slot the time origin lives in, and the per-realm install that stamps
   it. Released through the platform list's third column, so no host has a line to remember. */
void hr_time_init(JSContext *ctx);
void hr_time_free(void);

/* High Resolution Time §4 Time Origin's TIME ORIGIN of THIS realm's environment settings object, as a moment
   on the one virtual clock, COARSENED at the read. §4 stores the raw moment ("That moment is stored in that
   settings object's time origin.") and `coarsen time` picks its resolution from the environment's
   cross-origin isolated capability — a field HTML §7.2.2.6 defines over "window's associated DOCUMENT",
   which does not exist when the realm's install stamps the slot. So the write is the moment and the read is
   the coarsening; hr_time.c's install says what asking it the other way round cost. `get time origin timestamp`
   is the stored moment's SECOND reader and it is the operation BELOW, not this one — §4 gives it the moment as
   STORED, which is why two entry points read one slot rather than one being written in terms of the other. */
JSValue hr_time_origin(JSContext *ctx);   /* OWNED */

/* §4's GET TIME ORIGIN TIMESTAMP, given this realm as the global — "the duration from the estimated monotonic
   time of the Unix epoch to timeOrigin". That estimate is the AGENT's (§4 gives one to "each group of
   environment settings objects that could possibly communicate in any way") and is initialized by the first
   realm this agent builds. HR-TIME §7.2 timeOrigin attribute is its only reader — core/timing/performance.c.
   IT IS THE ONE VALUE IN THIS COMPONENT THE REAL WALL CLOCK REACHES, once per agent, and hr_time.c's
   declaration of the estimate says why that is the honest answer here and would be a fabrication anywhere else
   in this file: without it every document reports having been navigated to in January 1970, which is a
   plausible number rather than an absent one. OWNED. */
JSValue hr_time_origin_timestamp(JSContext *ctx);   /* OWNED */

/* §4's COARSEN TIME, given an unsafe moment on the monotonic clock. §4's second argument is the environment's
   cross-origin isolated capability, and `ctx` IS that environment — a caller passes the realm whose settings
   object the moment is FOR, never the realm the algorithm happens to be driven from. Answers a moment, never a
   duration — the spec coarsens the ABSOLUTE moment and subtracts the origin afterwards, and doing it the other
   way round would round a duration whose two ends are on the grid. OWNED, and the argument is a MOMENT and
   not a `double` — see the header note on why the coarsening is a derivation. */
JSValue hr_time_coarsen(JSContext *ctx, JSValueConst unsafe_moment);

/* §4's RELATIVE HIGH RESOLUTION TIME given an unsafe moment and a global object — coarsen the moment with this
   environment's capability, then take the duration from this environment's time origin to it. This is the
   operation HTML §8.1.7.3 steps 11, 14, 19 and 20 name in their own argument lists, and DOM §2.5 names for
   `timeStamp`; `ctx` IS the relevant global object those steps mean, so a per-document step must pass the
   DOCUMENT's realm and never the realm the algorithm happens to be driven from. OWNED. */
JSValue hr_time_relative(JSContext *ctx, JSValueConst unsafe_moment);

/* §4's CURRENT HIGH RESOLUTION TIME given a global object — the relative high resolution time of the unsafe
   shared current time, which in this engine is the event loop's virtual clock (core/timing/event_loop.h).
   WHAT IT RESOLVES TO: §4's grid, 0.1 ms for an ordinary environment and 0.005 ms for a cross-origin isolated
   one. THE CLOCK UNDERNEATH IT MOVES WITHIN ONE TASK, by the work the running flow retired — this line used to
   say the opposite ("two calls inside one task answer the SAME number however much work runs between them"),
   which was true until the clock got its second mover and was then a header telling its callers to expect a
   value the engine had stopped producing. Two calls inside one task differ by the work between the polls that
   bracket them; hr_time.c states at the unsafe shared current time exactly what a read observes and why the
   answer lags the current instruction by at most one yield poll. What has NOT changed is the other half: this
   is not a wall clock and a caller must not read it as one — the quantity is the flow's own path, which is
   what makes it identical on every schedule and across a park. */
JSValue hr_time_current(JSContext *ctx);   /* OWNED */

#endif
