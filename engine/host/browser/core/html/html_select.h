/* HTML §4.10.7 "The select element" — the members §4.10.7 declares over the OPTIONS COLLECTION rather than
 * over an attribute, and the overload that makes one of them mean two different algorithms.
 *
 * ONE PROBLEM: `remove` IS TWO OPERATIONS WITH ONE NAME. §4.10.7's IDL declares both, adjacent:
 *     [CEReactions] undefined remove(); // ChildNode overload
 *     [CEReactions] undefined remove(long index);
 * and states the dispatch in prose rather than by type: "The remove() method must act like its namesake
 * method on that same options collection when it has arguments, and like its namesake method on the
 * ChildNode interface implemented by the HTMLSelectElement ancestor interface Element when it has no
 * arguments." So the two entries are distinguished by ARITY ALONE, which is Web IDL §3.6 "Overload resolution
 * algorithm" steps 3-4 — "Initialize argcount to be min(maxarg, n)" then "Remove from S all entries whose
 * type list is not of length argcount" — and nothing later in §3.6 is reached, because one entry is left.
 *
 * WHY IT IS A COMPONENT AND NOT A ROW OF ANY TABLE. Until this existed, HTMLSelectElement declared no `remove`
 * at all, so `select.remove(0)` resolved up the prototype chain to DOM §4.2.8 Mixin ChildNode's `remove()` on
 * Element.prototype — which REMOVES THE SELECT ITSELF. A page pruning its own options destroyed its own
 * dropdown, and `remove()`'s own arity-0 declaration made that the CORRECT answer for the zero-argument call
 * at the same time, so nothing about the member looked wrong from outside: `typeof select.remove` and
 * `select.remove.length` read exactly as a browser's do, and only the one-argument call diverged.
 *
 * NOTHING A PAGE FEATURE-DETECTS MOVES. `remove` was already reachable on every select through Element, so
 * `typeof select.remove === "function"` was already true; Web IDL §3.7.7 Operations' `length` is "the length
 * of the shortest argument list in the entries in S" at argument count 0, which is 0 for `remove()` alone and
 * 0 for the pair (§2.5.8's own worked example shows the empty tuple such an entry contributes), so the number
 * is unchanged as well. What changes is WHICH ALGORITHM a one-argument call runs, and that `remove` becomes
 * an own property of HTMLSelectElement.prototype, which is where §3.7.7 puts an operation the interface
 * declares. This is therefore NOT the §NO-STUBS hazard of installing a name a guard then finds: the guard
 * already answered true, and the answer it gave was wrong.
 *
 * THE LIST OF OPTIONS IS NOT HERE AND MUST NOT BE COPIED HERE. §4.10.7's "to get the list of options" is
 * core/html/html_form.c's walk, which §4.10.7's selectedness setting algorithm and §4.10.22.4's entry list
 * are already stated over; this file asks it through html_form_select_option_list. The options COLLECTION
 * object is a separate subproblem — `[SameObject] readonly attribute HTMLOptionsCollection options` is an
 * interface this engine does not mint yet — and §2.6.4.3's remove-an-option needs none of it: its two reads
 * are "the number of nodes represented by collection" and "the indexth element in collection", and §4.10.7
 * defines that collection's filter as "the elements in the list of options", so both are questions about the
 * LIST and the object is an indirection rather than a state.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_SELECT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_SELECT_H

#include "quickjs.h"

/* The AGENT's half: §4.10.7's `remove` overload, declared once per runtime. */
void html_select_declare(JSContext *ctx);
/* §4.10.7's `remove` on HTMLSelectElement.prototype. Handed the prototype by core/html/html_element.c, which
   owns the table of which interface a tag wears, for the same reason §4.10.13's progress members are. */
void html_select_install(JSContext *ctx, JSValueConst proto);

#endif
