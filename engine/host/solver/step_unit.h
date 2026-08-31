/* WHERE A SCHEDULER STEP ENDED — the arm of flow_step a member last returned through, as a NAMED value.
 *
 * THE DEFECT THIS ENDS. flow_step has two dozen exits and it named the one it took in a file-scope
 * `const char *g_step_unit`, assigned a free-form string literal at each arm. That value had exactly two
 * readers and both were inside DFAILF MESSAGE BODIES — so the fact reached a person only on the runs that
 * ABORTED, and never on the runs a person compares. It is CLAUDE.md's computed-writer-with-no-reader defect
 * (the mirror of a defaulted field, and harder to see because the value is real), and what it cost is one
 * silence covering arms that take OPPOSITE work: a frontier looping in `queue-rendering-opportunity` or
 * `fire-due-timer` has unbounded periodic work and is a fidelity gap or a regression; one sitting in
 * `compile-program`/`resume-program` legitimately holds more script than it used to; one in the orphan arms is
 * seeding drives; one at `await-owed-reply` is parked on the host. Those are four different diffs, and
 * "no flow finished across the window" was the whole of what the gate could say about which.
 *
 * WHY IT IS AN ENUM AND NOT THE STRING IT WAS. A census row keyed on a free-form literal is a SECOND list of
 * the arms — one at the assignment, one at the reader — and two lists of one thing disagree eventually, which
 * is the same failure as a `@COLD` field renamed on one side. The list below is the only place an arm is
 * named: the enum constant, the string the diagnostics print, and the row the `@COLD` histogram carries are
 * all expansions of it, so an arm ADDED here appears in the census and in the abort text with nothing else to
 * remember, and an arm added anywhere ELSE cannot compile.
 *
 * `NONE` IS FIRST AND IS A REAL ANSWER, not a hole. A Flow is calloc'd, so a member that has never been handed
 * the thread reads `none` — which is a fact about that member (it pairs with the `@WFQ` census's `unrun`) and
 * not an absent measurement. It is ALSO the reset flow_step takes at its own entry, which is what makes the
 * assertion at the scheduler's convergence point able to fire: an arm that returns without naming itself
 * leaves `none` standing where a step just happened, and that crashes by name instead of silently attributing
 * the step to whatever the PREVIOUS one did.
 * SO ITS ROW READS DIFFERENTLY IN THE TWO HISTOGRAMS KEYED ON THIS LIST, and a reader who does not know that
 * will chase it. In the PER-MEMBER census (solver/cold.h's `step_units`) `none` is a real population: the
 * members that have never been handed the thread. In the LIFETIME one (solver/engine.h's EngineStepUnitRuns,
 * emitted as `stepUnitRuns`) it is ZERO for the whole life of every instance, because that census counts
 * RECORDED steps and the convergence point asserts a recorded step is never `none`. That zero is the assert
 * restated rather than a measurement, and the row is emitted anyway: both histograms are expansions of THIS
 * list, and a row dropped from one of them for being structurally zero would make the two key sets differ,
 * which is a difference every reader of either would then have to know about.
 *
 * THE PARTITION IS COMPLETE ON PURPOSE — every exit of flow_step is one of these, including the three that
 * return OWED without doing any work (`host-blocked`, `await-owed-reply`, `await-peer-operation`) and the one
 * that returns DONE (`finished`). Those four used to leave the previous arm's name standing, so a member
 * parked on the host was reported under whatever it last DID, which is precisely the attribution this
 * instrument exists to get right. */
#ifndef ENGINE_HOST_SOLVER_STEP_UNIT_H
#define ENGINE_HOST_SOLVER_STEP_UNIT_H

#include "check.h"

/* THE ONE LIST. `X(id, name)` — `id` becomes `STEP_UNIT_<id>` and `name` is what every reader prints and what
   the `@COLD` histogram keys its row on, so a rename here is a rename everywhere it is spoken. The names are
   the ones flow_step already used, verbatim, so a reader who has seen an abort text recognises the row. */
#define STEP_UNITS(X)                                                             \
    /* the member has never been stepped, and the reset flow_step enters at */    \
    X(NONE,               "none")                                                 \
    /* the arms that PERFORM work — a step here is progress by definition */      \
    X(FORK_PEER_ANSWER,   "fork-over-a-peer-answer")                              \
    X(FORK_DECLINED_REQ,  "fork-over-a-declined-request")                         \
    X(RESUME_PARKED,      "resume-parked-continuation")                           \
    X(LINK_CONNECTED,     "link-connected-time")                                  \
    X(ROUTED_DELIVERY,    "routed-delivery")                                      \
    X(ROUTED_NOT_MINE,    "routed-delivery-not-this-timeline")                    \
    X(CROSS_AGENT_OP,     "cross-agent-operation")                                \
    X(MICROTASK,          "microtask-checkpoint")                                 \
    X(DELIVER_REPLY,      "deliver-one-reply")                                    \
    X(SCHEME_FETCH,       "scheme-fetch-answered")                                \
    X(RUN_TASK,           "run-a-task")                                           \
    X(LIFECYCLE,          "document-lifecycle-stage")                             \
    X(RENDERING,          "queue-rendering-opportunity")                          \
    X(TIMER,              "fire-due-timer")                                       \
    /* NO `unhandled-rejection-notify` ROW, AND ITS ABSENCE IS A STATEMENT. HTML  */ \
    /* §8.1.4.7 Unhandled promise rejections' "notify about rejected promises" is */ \
    /* a step of "perform a microtask checkpoint", which HTML §8.1.7.3 Processing */ \
    /* model defines and the scheduler runs at its own seam — so it is not an arm */ \
    /* of flow_step and costs no pick. It WAS one, at the bottom of this ladder   */ \
    /* behind a per-flow drain that this fixture has never once completed.        */ \
    X(ORPHAN_SEED,        "seed-one-orphan-flow")                                 \
    X(ORPHAN_ROUTE,       "hand-a-parked-drive-its-function")                     \
    X(ORPHAN_RESUME,      "resume-a-parked-orphan-drive")                         \
    X(COMPILE_PROGRAM,    "compile-program")                                      \
    X(RESUME_PROGRAM,     "resume-program")                                       \
    /* …and the arms that perform NO work: three flavours of waiting, and done */ \
    X(AWAIT_FETCH_RECORD, "await-fetch-record")                                   \
    X(HOST_BLOCKED,       "host-blocked")                                         \
    X(AWAIT_OWED_REPLY,   "await-owed-reply")                                     \
    X(AWAIT_PEER,         "await-peer-operation")                                 \
    X(FINISHED,           "finished")

#define STEP_UNIT_ENUM(id, name) STEP_UNIT_##id,
typedef enum { STEP_UNITS(STEP_UNIT_ENUM) STEP_UNIT_N } StepUnit;
#undef STEP_UNIT_ENUM

/* HOW WIDE THE HISTOGRAM IS AS JSON, DERIVED FROM THE LIST RATHER THAN COUNTED BY HAND. solver/result.c sizes
   its census buffer as terms and says "RE-DO THE COUNTS WHEN YOU ADD A ROW … there is no way to be nearly
   right" — which is true and is exactly why THIS block's count may not be one of those: adding an arm here
   would silently truncate a document somebody has to re-derive a hand-typed number for. One row is
   `"<name>":<long>,` — a quote, the name, a quote, a colon, up to 20 digits of `long`, a comma — so 24
   characters beside the name; plus the two braces and the terminator. */
#define STEP_UNIT_WIDTH(id, name) + (sizeof(name) - 1) + 24
enum { STEP_UNITS_JSON_MAX = 2 STEP_UNITS(STEP_UNIT_WIDTH) + 1 };
#undef STEP_UNIT_WIDTH

/* THE NAME, FROM THE SAME LIST. A switch rather than a table indexed by the enum, so the two cannot get out of
   step by an entry being inserted in one and appended to the other; it is generated from `STEP_UNITS`, so it
   is complete by construction and a missing case is not expressible. `static inline` because the whole body is
   the list and every translation unit that prints one should print the same bytes — there is no state here to
   have one copy of. */
#define STEP_UNIT_CASE(id, name) case STEP_UNIT_##id: return name;
static inline const char *step_unit_name(StepUnit u)
{
    switch (u) { STEP_UNITS(STEP_UNIT_CASE) case STEP_UNIT_N: break; }
    DFAIL("a scheduler step reported an arm that is not in solver/step_unit.h's list — the enum and the name "
          "are two expansions of ONE macro, so a value outside it did not come from an assignment in "
          "flow_step; it is a cast, an uninitialised read, or a field this value was stored in too narrow to "
          "hold it");
    return "(not a step unit)";
}
#undef STEP_UNIT_CASE

#endif
