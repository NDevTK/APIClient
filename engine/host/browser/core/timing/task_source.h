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
 * array wins: it is the sentence quoted above.
 *
 * THIS LINE READ `the only repair is that a source is in ONE queue`, AND THAT ABSOLUTE IS WITHDRAWN — REWRITTEN
 * RATHER THAN DELETED, BECAUSE THE REASONING ABOVE IT IS EXACT AND A READER WILL RE-DERIVE THE CONCLUSION FROM
 * IT. One queue per source IS a repair and it is the NARROWER of the two available. What the two orderings
 * above rule out is an order taken over the CARRIERS; an order taken over ARRIVAL satisfies both at once,
 * because within one source arrival order IS queue order, so a source in two arrays comes out in the order it
 * was queued whichever array holds each item. That is what solver/flow.c's g_work_seq is: ONE clock stamping
 * both of a flow's carriers, read by solver/engine.c's task ladder.
 * THE DIFFERENCE IS NOT TASTE AND IT IS WHY THE ABSOLUTE MATTERED. Moving a producer discharges §8.1.7.1 for
 * the one source moved and leaves the NEXT author of a producer to get it right again — and this header's own
 * next paragraph says why nobody can be relied on to: a producer added later states nothing, nothing notices,
 * and the enumeration has to be re-derived by reading every call site. A shared clock discharges it for every
 * source at once, including a source no producer has written yet, which is the population that argument is
 * about. The declarations below stay exactly as load-bearing: they are what makes `is any source in two
 * queues` a grep, and the audit is worth having whether or not the order needs the answer.
 * AND THE ABSOLUTE WAS THE ONE PART OF THIS FILE A READER COULD ACT ON WRONGLY: it reads as a standing
 * instruction to go and move a producer, which is a behaviour change to a spec algorithm's queueing point made
 * to satisfy a scheduling property the scheduler can hold on its own.
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
    /* …AND THE TASK WHOSE OWN STANDARD QUEUES IT AND NAMES NO SOURCE. This is a POSITIVE statement about the
       ALGORITHM and is not the sentinel one member up: the producer read its standard, found the words "queue a
       task" with nothing after them, and says so. CSP §5.5 "Report a violation" is the shape — "Queue a task to
       run the following steps", whose own Note explains only WHY it is queued ("to ensure that the event
       targeting and dispatch happens after JavaScript completes execution of the task responsible for a given
       violation") and never on what. Such a task cannot be in two queues FOR ONE SOURCE, because it has no
       source to be in two queues with, so §8.1.7.1's ordering rule does not reach it — the same standing as the
       two members above and for a third reason. Inventing a source for it would order it against the page's
       real tasks by a fact nobody stated, which is §@H's line between a value the code determined and a value
       made up to satisfy a gate. */
    TASK_SOURCE_UNNAMED_BY_ITS_STANDARD,
    /* HTML §8.1.7.4 "Generic task sources"' DOM manipulation task source — "This task source is used for features
       that react to DOM manipulations, such as things that happen in a non-blocking fashion when an element is
       inserted into the document." */
    TASK_SOURCE_DOM_MANIPULATION,
    /* HTML §8.1.7.4 "Generic task sources"'s user interaction task source — "This task source is used for features that react to user
       interaction, for example keyboard or mouse input." */
    TASK_SOURCE_USER_INTERACTION,
    /* §8.1.7.4 "Generic task sources"' networking task source — "This task source is used for features that
       trigger in response to network activity." A program whose bytes came from a response is this: §8.1.7.1
       lists the case in its own words ("When an algorithm fetches a resource, if the fetching occurs in a
       non-blocking fashion then the processing of the resource once some or all of the resource is available
       is performed by a task"). */
    TASK_SOURCE_NETWORKING,
    /* §8.1.7.4's navigation and traversal task source — "This task source is used to queue tasks involved in
       navigation and history traversal." */
    TASK_SOURCE_NAVIGATION_AND_TRAVERSAL,
    /* HTML §8.1.7.4 "Generic task sources"'s rendering task source — "This task source is used solely to update the rendering." */
    TASK_SOURCE_RENDERING,
    /* AND THE SOURCES A SINGLE STANDARD DEFINES FOR ITSELF. HTML §8.1.7.4 "Generic task sources"'s five are the ones "used by a number of
       mostly unrelated features"; every other source belongs to one algorithm's own specification, which is
       where its name is defined and where it must be read from. They are listed here rather than collapsed
       into the generic five because §8.1.7.1's guarantee is about a source and a collapsed one would put two
       standards' tasks in one queue by this file's decision rather than by anybody's standard. */
    /* HTML §8.7 "Timers"' timer task source — step 9's completionStep is "an algorithm step which queues a
       global task on the timer task source given global to run task". */
    TASK_SOURCE_TIMER,
    /* HTML §9.3.3 "Posting messages"' posted message task source — "Queue a global task on the posted message
       task source given targetWindow to run the following steps". */
    TASK_SOURCE_POSTED_MESSAGE,
    /* HTML §9.4.4 "Message ports"' port message queue, which is a task source PER PORT: "Each MessagePort
       object also has a task source called the port message queue" and "When a port's port message queue is
       enabled, the event loop must use it as one of its task sources." One enumerator covers every port for the
       same reason the engine has one task queue — what a per-port source decides is the order two ports drain
       in, which this engine does not model, and what it decides that IS modelled is that a port delivery is a
       TASK. */
    TASK_SOURCE_PORT_MESSAGE_QUEUE,
    /* HTML §4.8.11 "Media elements"' media element event task source — "Each media element has a unique media
       element event task source." Per element, and one enumerator, for the port message queue's reason. */
    TASK_SOURCE_MEDIA_ELEMENT_EVENT,
    /* File API §6.1 "The File Reading Task Source" — the section is the definition. */
    TASK_SOURCE_FILE_READING,
    /* IndexedDB §4 "API"'s database access task source — "The task source for these tasks is the database
       access task source. To queue a database task, perform queue a task on the database access task source". */
    TASK_SOURCE_DATABASE_ACCESS,
    /* Cooperative Scheduling of Background Tasks' idle-task task source — "Queue a task on the queue associated
       with the idle-task task source". That standard's maintained edition is unrendered ReSpec source with no
       section numbers in the bytes a fetch returns, so it is cited by algorithm name here and carries no number
       for the same reason engine/citegen.mjs records it as a foreign row. */
    TASK_SOURCE_IDLE_TASK,
    /* Performance Timeline §5.3 "Queue the PerformanceObserver task" — "The task source for the queued task is
       the performance timeline task source." */
    TASK_SOURCE_PERFORMANCE_TIMELINE,
    /* Intersection Observer §3.2.4 "Queue an Intersection Observer Task" — "The IntersectionObserver task
       source is a task source used for scheduling tasks to §3.2.5 Notify Intersection Observers." */
    TASK_SOURCE_INTERSECTION_OBSERVER,
    /* Permissions §3.4 "Permissions task source" — "The permissions task source is a task source used to
       perform permissions-related tasks in this specification." */
    TASK_SOURCE_PERMISSIONS
} TaskSource;

/* IS §8.1.7.1's ORDERING RULE ABOUT THIS WORK ITEM? Only a task has a source, so only a task can have one in
   two queues. The two non-task answers are not degrees of the same thing and neither is a hole: one says the
   causing algorithm runs this in place, the other says no algorithm of the standard queued it. */
static inline int task_source_is_task(TaskSource s)
{
    switch (s) {
    case TASK_SOURCE_UNSTATED:
    case TASK_SOURCE_NOT_A_TASK:
    case TASK_SOURCE_SOLVER_CANDIDATE:
    case TASK_SOURCE_UNNAMED_BY_ITS_STANDARD:
        return 0;
    default:
        return 1;
    }
}

/* MAY A WORK ITEM DECLARING THIS BE PUT ON A TASK QUEUE AT ALL — a DIFFERENT question from the one above, and
   the two are split rather than merged because one value answers both and they are not the same answer.
   `task_source_is_task` asks whether §8.1.7.1's ONE-SOURCE-ONE-QUEUE RULE is about this work item, which is a
   question about the SOURCE; this asks whether the producer stated anything at all, which is a question about
   the PRODUCER. A task whose standard names no source (CSP §5.5) and a fire the standard runs in place that
   this engine queues anyway are both legitimate answers to the second and both `no` to the first — so a single
   predicate would either refuse them, which is wrong, or admit the never-written sentinel, which is the whole
   thing the sentinel exists to catch. Splitting them is §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS's own cure:
   one FACT, two QUESTIONS asked of it, rather than two bits that can disagree. */
static inline int task_source_stated(TaskSource s)
{
    return s != TASK_SOURCE_UNSTATED;
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
    case TASK_SOURCE_UNNAMED_BY_ITS_STANDARD:  return "unnamed-by-its-standard";
    case TASK_SOURCE_DOM_MANIPULATION:         return "dom-manipulation";
    case TASK_SOURCE_USER_INTERACTION:         return "user-interaction";
    case TASK_SOURCE_RENDERING:                return "rendering";
    case TASK_SOURCE_TIMER:                    return "timer";
    case TASK_SOURCE_POSTED_MESSAGE:           return "posted-message";
    case TASK_SOURCE_PORT_MESSAGE_QUEUE:       return "port-message-queue";
    case TASK_SOURCE_MEDIA_ELEMENT_EVENT:      return "media-element-event";
    case TASK_SOURCE_FILE_READING:             return "file-reading";
    case TASK_SOURCE_DATABASE_ACCESS:          return "database-access";
    case TASK_SOURCE_IDLE_TASK:                return "idle-task";
    case TASK_SOURCE_PERFORMANCE_TIMELINE:     return "performance-timeline";
    case TASK_SOURCE_INTERSECTION_OBSERVER:    return "intersection-observer";
    case TASK_SOURCE_PERMISSIONS:              return "permissions";
    }
    return "unknown";
}

#endif
