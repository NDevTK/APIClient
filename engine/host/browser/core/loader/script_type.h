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

#endif
