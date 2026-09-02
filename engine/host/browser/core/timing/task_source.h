/* HTML §8.1.7.1 "Definitions" — A TASK'S `source` FIELD, AS A VALUE THE PRODUCER STATES.
 *
 * §8.1.7.1 makes a task a struct, and one of its four fields is a source: "One of the task sources, used to
 * group and serialize related tasks". That field is not bookkeeping on the side of the queue — it is what the
 * same section's ordering guarantee is written over: "For each event loop, every task source must be
 * associated with a specific task queue", and then, of the freedom §8.1.7.3 "Processing model" step 2.1 leaves
 * a user agent between queues, "Note that in this setup, the processing model still enforces that the user
 * agent would never process events from any one task source out of order."
 *
 * SO THE INVARIANT IS ABOUT A SOURCE AND NEVER ABOUT A CARRIER, AND THAT DISTINCTION IS WHY THIS FILE EXISTS.
 * A flow holds its work in arrays that partition by what a work item IS — a PROGRAM (solver/flow.h's `dyn`),
 * a queued C callback (`jobs`), an answered host request (`pending`) — where §8.1.7.1 partitions by where the
 * work item CAME FROM. Those two partitions are unrelated, and where one source lands in two arrays no
 * arrangement of the arrays can order it, because whichever array is consulted first answers both of
 *     A queued, then B      must run A then B
 *     B queued, then A      must run B then A
 * the same way round. That is not a scheduling preference to tune and it is not repaired by choosing which
 * array wins: it is the sentence quoted above, and the only repair is that a source is in ONE queue.
 *
 * IT IS A VALUE AND NOT A COMMENT BECAUSE A COMMENT CANNOT BE ASKED. Every producer in this engine already
 * stated its source in prose beside its call — and prose is invisible to the thing it is queueing into, so a
 * producer added later states nothing, nothing notices, and the enumeration that would answer "is any source
 * in two queues" has to be re-derived by reading every call site again. Each of those readings has been done
 * at least twice already and one of them was wrong. The value travels to the queueing point instead, which
 * asserts what it can (solver/engine.c's engine_queue_into).
 *
 * WHAT IS NOT HERE IS A TABLE OF WHICH CARRIER SERVES WHICH SOURCE. That would be a claim about this tree
 * written where nothing re-checks it, which is exactly the shape that goes stale between the diff that writes
 * it and the diff that moves a producer. What answers that question is the DECLARATIONS themselves: a source
 * reaches a carrier if and only if some producer on that carrier names it, which a grep for the enumerator
 * answers and no restatement can contradict.
 *
 * IT IS ITS OWN HEADER BECAUSE BOTH HALVES READ IT, for core/loader/script_type.h's reason: the browser half
 * knows which spec algorithm queued a work item, the SOLVER's scheduler is what orders one against another,
 * and an enum with no dependencies can live where both include it. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_TASK_SOURCE_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_TASK_SOURCE_H

typedef enum {
    /* NEVER WRITTEN BY A PRODUCER — the named sentinel a queueing point asserts against, so a caller that was
       added without stating a source cannot masquerade as one that has nothing to say. Zero, because a field
       or a struct that is memset is then honestly unstated rather than silently claiming the first real
       answer in the list. */
    TASK_SOURCE_UNSTATED = 0,
    /* NOT A TASK, and this is a POSITIVE STATEMENT about the work item rather than the absence of one. §8.1.7.1
       gives a source to a TASK; a great deal of what a flow runs is not a task at all, because the spec runs it
       inside the algorithm that caused it. HTML §4.12.1.1 "Processing model" ends "prepare the script element"
       with "Otherwise, immediately execute the script element el, even if other scripts are already executing";
       ECMAScript §19.2.1.1 PerformEval evaluates the body inside the call expression; a document's own inline
       scripts are run by the parse that reached them, which §8.1.7.1 describes as work a task DOES ("The HTML
       parser tokenizing one or more bytes, and then processing any resulting tokens, is typically a task")
       rather than as a task apiece. A work item declaring this is outside the one-source-one-queue rule
       because it has no source to be in two queues with. */
    TASK_SOURCE_NOT_A_TASK,
    /* …AND THE WORK ITEM NO SPEC ALGORITHM QUEUED AT ALL. An @S candidate is the solver re-firing a sink with
       one attacker value substituted; it is the page's own code, but nothing in HTML asked for it, so it has
       no §8.1.7.1 source and must not be given one — a source is what orders a task against the page's other
       tasks, and inventing one here would order a program the page never queued against programs it did.
       What the candidate DOES carry about its position is the sink's own semantics, which is already its
       DynPos (solver/engine.h): an eval sink is PerformEval and runs inside the call expression, a markup
       sink's auto-firing handler and a URL sink's `javascript:` navigation take the tail. */
    TASK_SOURCE_SOLVER_CANDIDATE,
    /* §8.1.7.4 "Generic task sources"' networking task source — "This task source is used for features that
       trigger in response to network activity." A program whose bytes came from a response is this: §8.1.7.1
       lists the case in its own words ("When an algorithm fetches a resource, if the fetching occurs in a
       non-blocking fashion then the processing of the resource once some or all of the resource is available
       is performed by a task"). */
    TASK_SOURCE_NETWORKING,
    /* §8.1.7.4's navigation and traversal task source — "This task source is used to queue tasks involved in
       navigation and history traversal." */
    TASK_SOURCE_NAVIGATION_AND_TRAVERSAL
} TaskSource;

/* IS §8.1.7.1's ORDERING RULE ABOUT THIS WORK ITEM? Only a task has a source, so only a task can have one in
   two queues. The two non-task answers are not degrees of the same thing and neither is a hole: one says the
   causing algorithm runs this in place, the other says no algorithm of the standard queued it. */
static inline int task_source_is_task(TaskSource s)
{
    return s == TASK_SOURCE_NETWORKING || s == TASK_SOURCE_NAVIGATION_AND_TRAVERSAL;
}

/* THE SOURCE'S OWN NAME, for the assert that names it. A `@WHY` reading "a work item's source is one this
   carrier does not serve" tells its reader nothing they can act on; the name is what makes the crash say WHICH
   source, and it is the same string a grep for the enumerator finds. */
static inline const char *task_source_name(TaskSource s)
{
    switch (s) {
    case TASK_SOURCE_UNSTATED:                 return "unstated";
    case TASK_SOURCE_NOT_A_TASK:               return "not-a-task";
    case TASK_SOURCE_SOLVER_CANDIDATE:         return "solver-candidate";
    case TASK_SOURCE_NETWORKING:               return "networking";
    case TASK_SOURCE_NAVIGATION_AND_TRAVERSAL: return "navigation-and-traversal";
    }
    return "unknown";
}

#endif
