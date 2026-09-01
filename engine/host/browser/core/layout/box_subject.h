/* THE SUBJECT OF A LAYOUT CRASH — the box, the node, or the computed value that a message about to abort is
 * ABOUT, as opposed to the function that noticed.
 *
 * WHY THIS IS A COMPONENT AND NOT A STATIC IN EACH FILE. §AN-ASSERT-THAT-NAMES-A-REMEDY's test is a COUNT:
 * once more call sites can reach an abort than a reader would read by hand, the ADDRESS is part of the assert
 * rather than decoration on it. Layout's aborts are the extreme form of that shape, and for a reason a fan-in
 * number cannot show: a single member — one `element.scrollWidth`, one `getClientRects()` — walks a subtree
 * and asks the same question once per BOX, so the number of possible subjects is the document's box count and
 * no count of call sites bounds it. A symbolized frame list already names the ASKER, which is why threading a
 * `__FILE__`/`__LINE__` from those callers buys nothing here and would be captured at a handful of forwarding
 * functions besides. The SUBJECT is the half that gets guessed, and stopping that guess is the whole of what
 * these three functions are for.
 *
 * IT CANNOT ASSERT, AND THAT IS STRUCTURAL RATHER THAN A PROMISE: box_subject.c DOES NOT INCLUDE `check.h`.
 * A helper a `DFAILF` calls must be TOTAL, because an assert firing during message composition does not report
 * a SECOND defect — it REPLACES the first, and the reader loses the box, the container and the remedy at once.
 * So every arm here answers with a SENTENCE rather than refusing, and the sentences are distinguishable
 * because the questions are: "(no element)" says the caller had nothing to name, "(no local name)" says it had
 * a node neither the parser nor `createElement` minted, and "(no computed value)" says the cascade did not
 * answer. None of them is a `?:` past a broken invariant — the invariant still has its own crash, at the site
 * that DEPENDS on the value rather than at the site that is printing it.
 * THE COROLLARY IS A RULE FOR CALLERS, and it is the one thing a file adopting this component must not undo.
 * A layout file typically wraps `css_computed_value` in its own predicate that DCHECKs the cascade answered.
 * That assert is CORRECT where the file reads a property to compute geometry with, and is exactly the
 * replacement above where it reads one to name a box — so a message reaches this component and never that
 * predicate. The same rule, sharper, is why nothing here asks whether a box is REPLACED: the entry that
 * answers that aborts by name for the very element kinds whose own gap a layout message is usually reporting,
 * so asking it would substitute one component's gap for another's.
 *
 * THE OWNERSHIP CONTRACT IS A COPY INTO THE CALLER'S BUFFER, and it is that BECAUSE the callers cannot be
 * known. The alternative is to hand back the cascade's own pointer and leak it, on the argument that the
 * process is one `abort()` away. That argument is TRUE at a `DFAILF` site and it is a PRECONDITION ON THE
 * CALLER — one a shared component cannot check, cannot state to a compiler, and cannot enforce on a caller it
 * has never seen, which is the shape of every contract this project has had to learn by miscompilation. So the
 * allocation is made and released inside the one function that made it, on the aborting path and the
 * release-build path alike, and no crash site is asked to free anything it is aborting past. It costs nothing:
 * the caller supplies a buffer it had to supply for the name regardless.
 *
 * A LOCAL NAME IS WRITTEN THROUGH `%.*s` WITH LEXBOR'S OWN LENGTH rather than as a C string, because it is a
 * length-and-pointer pair with no promise of a terminator. C99 §7.19.6.1 "The fprintf function" is what makes
 * that safe: the argument "shall be a pointer to the initial element of an array of character type", with a
 * precision "no more than that many bytes are written", and the array "shall contain a null character" only
 * "if the precision is not specified or is greater than the size of the array".
 *
 * NAMED RESIDUAL — THE TOTALITY ABOVE IS THIS COMPONENT'S OWN AND IS NOT ITS CALLEE'S. `css_computed_value`
 * answers a NAMED set of keyword-valued properties and ABORTS outside it: for a length-valued name, for a name
 * it does not model, for `line-height`, and for a `transform` whose specified value is not `none`. Reaching it
 * directly rather than through a caller's own predicate removes THAT predicate's extra assert and removes none
 * of these. WHAT IS NOT COVERED: a property name outside the set that entry answers. WHAT THE NEXT DIFF
 * BUILDS: that answering set as a predicate `css_computed_value.h` EXPORTS, which is the only place it can be
 * written — `css_computed_models` ADMITS `line-height` and `transform`, so a guard composed here out of the
 * two predicates that file already exports would be partial while reading as total, and naming those two by
 * hand would be a second copy of a rule the cascade decides. HOW ITS ABSENCE WOULD SHOW: a caller passing such
 * a name aborts INSIDE message composition, and its reader is handed the cascade's invariant in place of the
 * layout defect being reported — this component's own failure mode, wearing the callee's file and line. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_SUBJECT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_SUBJECT_H
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* `<div> (display `block`)` — the element and the one computed value every layout remedy is stated against.
   Answers a sentence for a NULL element and for one with no local name; never touches `buf` in either case. */
const char *box_subject(lxb_dom_element_t *el, char *buf, size_t cap);

/* THE SAME, FOR A NODE THAT MAY NOT BE AN ELEMENT. A layout question is as often about a node as about a box —
   a run is delimited over a container's whole child list, a containing block is decided by what a node's
   PARENT is, and a walk over "the element's descendants' boxes" descends through text as well as elements — so
   at those sites "(no element)" would be a WRONG answer rather than a missing one, and the node KIND is the
   fact the reader needs. */
const char *box_subject_node(lxb_dom_node_t *n, char *buf, size_t cap);

/* One computed value, copied into the caller's buffer and the cascade's string released here. See the residual
   above for which property names the callee answers. */
const char *box_subject_computed(lxb_dom_element_t *el, const char *name, char *buf, size_t cap);

#endif
