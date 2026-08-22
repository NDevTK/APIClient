/* HTML §4.12.1 "prepare the script element" — EL'S TYPE.
 *
 * The script block's type string decides which of four algorithms an element's content is: a classic script, a
 * module script, an import map, or a set of speculation rules. It is a fact about the ELEMENT, computed before
 * anything is fetched or created, and everything downstream branches on it — §8.1.3.1's "creating a classic
 * script" vs "creating a module script", then §8.1.3.3's "run a classic script" (which produces a COMPLETION)
 * vs "run a module script" (Evaluate(), which produces a PROMISE, which is why a top-level `await` is
 * observable at all).
 *
 * IT EXISTS BECAUSE THE PRODUCER USED TO DROP IT. `script_is_exec(el, &is_mod)` COMPUTED the module bit and
 * returned a bare 1, and both of its callers passed a local they never read; `DocScripts` had nowhere to keep
 * it and no other caller existed. So a `<script type=module>` arrived at the compiler indistinguishable from a
 * classic one, was compiled with JS_EVAL_TYPE_GLOBAL, and its top-level `await` came back a SyntaxError from a
 * parser that is perfectly fine. A computed fact with no consumer is the defect; the fact is now the value the
 * producer RETURNS, and it rides the document's script sequence to the one place that compiles a program.
 *
 * IT IS ITS OWN HEADER BECAUSE BOTH HALVES READ IT. The browser half computes it from the DOM; the SOLVER's
 * scheduler consumes it at the compile. document_scripts.h pulls in Lexbor, and the solver may not depend on
 * the DOM library (engine.c registers a callback for one statistic rather than include the DOM's header, and
 * says why at that line). An enum with no dependencies can live where both include it. */
#ifndef ENGINE_HOST_BROWSER_LOADER_SCRIPT_TYPE_H
#define ENGINE_HOST_BROWSER_LOADER_SCRIPT_TYPE_H

typedef enum {
    /* HTML's null — "No script is executed, and el's type is left as null." A data block (application/json,
       ld+json, a template) is this, and so is any type string none of the four matches name. */
    SCRIPT_TYPE_NONE = 0,
    SCRIPT_TYPE_CLASSIC,
    SCRIPT_TYPE_MODULE,
    SCRIPT_TYPE_IMPORTMAP,
    SCRIPT_TYPE_SPECULATIONRULES
} ScriptType;

/* THE TWO OF THE FOUR THAT EXECUTE JAVASCRIPT. An import map and a set of speculation rules are parsed DATA —
   §4.12.1's "execute the script element" registers them on the relevant global and evaluates nothing — so they
   are neither programs of the document nor part of its JS identity. */
static inline int script_type_executes(ScriptType t)
{
    return t == SCRIPT_TYPE_CLASSIC || t == SCRIPT_TYPE_MODULE;
}

/* WHEN THE ELEMENT RUNS — the LAST steps of §4.12.1's "prepare the script element", which sort the element into
 * one of the Document's four script queues (or run it on the spot). It is a second fact about the element that
 * only the element knows, and it is beside `type` because the two answer together: the tail branches on `async`,
 * `defer`, `force async`, `parser-inserted` AND on the type.
 *
 * IT IS MISSING FROM THE INVENTORY AND THAT IS WHY EVERY EXTERNAL SCRIPT LOOKED THE SAME. A consumer holding
 * only "external" has to treat every one of them as ordered, so it asserted on a document whose order it was in
 * fact keeping (`<script src=x async></script><script>…</script>` runs the inline FIRST — the parser is never
 * blocked) and asserted nothing about the one it silently reorders (two ORDERED externals, whose replies join
 * the flow in ARRIVAL order). A parser-inserted external script takes three different schedules, so a mechanism
 * that fixes the ordering cannot be built before the classification exists: forcing an `async` script into a
 * document-ordered sequence is a different wrong answer. */
typedef enum {
    /* No fetch is owed, so §4.12.1's tail ends at "immediately execute the script element": an inline classic
       script, at its own parse position. That same tail routes one to the pending parsing-blocking script
       instead when the parser document "has a style sheet that is blocking scripts" — this engine loads no
       style sheets, so nothing of it can be blocking scripts and that arm is unreachable rather than skipped. */
    SCRIPT_SCHED_IMMEDIATE = 0,
    /* The parser document's `pending parsing-blocking script`: §13.2.6.4.8 blocks the tokenizer and spins the
       event loop until it is ready, so EVERYTHING later in the document waits for its fetch AND evaluation. */
    SCRIPT_SCHED_PARSER_BLOCKING,
    /* The `list of scripts that will execute when the document has finished parsing` (a `defer` external
       classic script, and every non-async module script). §13.2.7 runs the list IN ORDER, after the parse and
       before DOMContentLoaded. */
    SCRIPT_SCHED_WHEN_PARSED,
    /* The `list of scripts that will execute in order as soon as possible` — not parser-inserted and not force
       async, i.e. an element script created and then given `async = false`. Ordered against each other only. */
    SCRIPT_SCHED_IN_ORDER_ASAP,
    /* The `set of scripts that will execute as soon as possible` — a SET, and §13.2.7 waits for it only before
       the load event. There is NO order to keep: any arrival order is a correct one. */
    SCRIPT_SCHED_ASAP
} ScriptSchedule;

/* Is the element's position ORDERED against another script's? Everything except the ASAP set, which is a set —
   §13.2.7 waits for that set only before the load event and never fixes an order within it. */
static inline int script_sched_is_ordered(ScriptSchedule s)
{
    return s != SCRIPT_SCHED_ASAP;
}

/* HTML §13.2.7 "The end" — THE ORDER A DOCUMENT RUNS ITS SCRIPT QUEUES IN, as a rank over the schedules above.
 * §4.12.1 says which queue an element joins; this says when that queue runs, and the two together are the
 * document's program order. The three ranks are the standard's own steps and not a preference:
 *   0  PARSE POSITION — an inline classic script ("immediately execute the script element") and the `pending
 *      parsing-blocking script`, which §13.2.6.4.8 'The "text" insertion mode' blocks the tokenizer for.
 *   1  "While the list of scripts that will execute when the document has finished parsing is not empty …
 *      execute the script element given by the first script in the list" — after the parse, before
 *      DOMContentLoaded.
 *   2  "Spin the event loop until the set of scripts that will execute as soon as possible AND the list of
 *      scripts that will execute in order as soon as possible are empty" — before the load event. §13.2.7 waits
 *      for the two together and orders NEITHER against the other, so they share one rank; telling them apart is
 *      `sched`'s job and not the rank's, because what differs is whether the element holds a POSITION
 *      (script_sched_is_ordered) rather than when its queue is due. */
static inline int script_sched_run_rank(ScriptSchedule s)
{
    if (s == SCRIPT_SCHED_WHEN_PARSED)                                return 1;
    if (s == SCRIPT_SCHED_IN_ORDER_ASAP || s == SCRIPT_SCHED_ASAP)    return 2;
    return 0;
}
#define SCRIPT_RUN_RANK_N 3

#endif
