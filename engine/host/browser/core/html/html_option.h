/* THE `option` ELEMENT'S OWN STATE — HTML §4.10.10 "The option element": its SELECTEDNESS, its DIRTINESS, the
 * `selected` IDL attribute those two decide, and Web IDL §3.7.2 "Legacy factory functions"'s `Option`.
 *
 * ONE PROBLEM: WHAT AN OPTION IS SELECTED. §4.10.10 states it as two booleans that are not the `selected`
 * CONTENT ATTRIBUTE and are not each other:
 *   - "The selectedness of an option element is a boolean state, initially false. Except where otherwise
 *     specified, when the element is created, its selectedness must be set to true if the element has a
 *     selected attribute."
 *   - "The dirtiness of an option element is a boolean state, initially false. It controls whether adding or
 *     removing the selected content attribute has any effect."
 * While dirtiness is false the two track the attribute exactly, which is why the attribute alone was a correct
 * model for as long as nothing in this build could make dirtiness true. §4.10.10's `selected` SETTER makes it
 * true — "it must set the element's selectedness to the new value, set its dirtiness to true, and then cause
 * the element to ask for a reset" — and §4.10.10's own legacy factory function makes selectedness DISAGREE
 * with the attribute without touching dirtiness at all: its step 5 sets the `selected` attribute when
 * `defaultSelected` is true and its step 6 then sets selectedness "to false (even if defaultSelected is true)".
 * So `new Option("a", "b", true, false)` is an option whose `defaultSelected` is true, whose serialized markup
 * carries `selected`, and whose `selected` is FALSE — a state one boolean cannot hold.
 *
 * THE SLOT AND THE ATTRIBUTE ARE ONE READ, NOT TWO. Selectedness lives in the element's per-flow PROPERTY slot
 * (solver/attr_shadow.h's ATTR_SLOT_PROPERTY, the same place §4.10.5.1.15's checkedness and §4.10.18.1's dirty
 * value flag live), and an ABSENT slot is the positive statement "nothing has overridden the attribute yet" —
 * so the read falls through to the attribute rather than to a default this file invented. That is what makes
 * the parser's `<option selected>` right with no write anywhere, and it is why a write that would not change
 * the answer records nothing: the slot rides the COW delta, and a no-op write repeated at every read would
 * grow the delta by the number of times the page LOOKED.
 *
 * WHY THE RESET IS RUN AT THE READ. §4.10.7 "The select element" names several moments at which a select runs
 * its SELECTEDNESS SETTING ALGORITHM — an option asks for a reset, options are added or removed, `multiple`
 * changes — and this engine has insertion steps for none of them. The algorithm is IDEMPOTENT and it writes
 * only the selectedness slot, which nothing outside this file and html_form.c can observe, so running it at
 * every READ of selectedness is observationally identical to having run it at each of those moments and is not
 * a narrowing: there is no point between two reads at which a page could see the difference. It is also what
 * keeps ONE implementation of the algorithm — html_form.c's, over the list of options it already owns —
 * instead of the read-time normalization that used to stand beside it and answer for `select.selectedOptions`
 * while a sibling `option.selected` read answered the un-normalized state.
 *
 * WHAT `new Option(...)` IS FOR IN THIS ENGINE. It is how a page builds a `select`'s contents out of data it
 * fetched — `new Option(row.label, row.id)` in a loop over an API reply is the ordinary spelling — so the
 * `value` argument is very often a value the run LEARNED, and it crosses as that value (element_attr_set_value
 * carries the whole triple into the (element, name) shadow) rather than as a C string this file formats. The
 * name was on no global, so every one of those loops threw a ReferenceError at its first iteration and the
 * flow stopped there. */
#ifndef APICLIENT_CORE_HTML_HTML_OPTION_H
#define APICLIENT_CORE_HTML_HTML_OPTION_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §4.10.10's SELECTEDNESS, read RAW — the per-flow slot if one has been written, the `selected` content
   attribute otherwise. RAW means "without asking §4.10.7's selectedness setting algorithm to run first", which
   is what the algorithm ITSELF must use and what every other reader must not: a reader that skips the reset
   sees the state before a single-select has collapsed two selected options to one. The IDL getter and
   html_form_selected_options both run the reset and then read this. */
bool html_option_selectedness(const lxb_dom_element_t *opt);

/* SET §4.10.10's selectedness. A write that would not change the answer records NOTHING — the slot rides the
   COW delta and §4.10.7's algorithm runs at every read, so an unconditional write would add a delta entry per
   option per read of a state nobody changed. */
void html_option_set_selectedness(JSContext *ctx, lxb_dom_element_t *opt, bool on);

/* §4.10.10's DIRTINESS — "a boolean state, initially false. It controls whether adding or removing the selected
   content attribute has any effect." Set by the `selected` setter and by nothing else in this build; §4.10.7's
   `select.value` and `select.selectedIndex` setters and the user's "pick an option" are its other three writers
   and none of them exists here yet. */
bool html_option_dirtiness(const lxb_dom_element_t *opt);

/* §4.10.10's ATTRIBUTE CHANGE STEPS for `selected`: "Whenever an option element's selected attribute is added,
   if its dirtiness is false, its selectedness must be set to true. Whenever an option element's selected
   attribute is removed, if its dirtiness is false, its selectedness must be set to false." Registered on
   core/dom/element.c's element_attr_changed beside its neighbours, and for the same reason they are there: a
   content attribute has more than one spelling (`opt.defaultSelected = true`, `setAttribute`, `removeAttribute`,
   an `innerHTML` reparse) and an IDL setter answers for exactly one of them. ADDED and REMOVED are what the
   steps turn on, so both the old value and the new one cross — a `selected=""` overwritten with `selected="x"`
   is neither. */
void html_option_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *old_val, const char *val);

/* §4.10.10's "cause the element to ask for a reset" — §4.10.7: "When an option element's selectedness is set to
   true, ... or when an option element in the list of options asks for a reset, then run that select element's
   selectedness setting algorithm." A no-op for an option in no select's list of options, which is what a
   freshly constructed one is. */
void html_option_ask_for_a_reset(JSContext *ctx, lxb_dom_element_t *opt);

/* §4.10.10's AGENT-WIDE DECLARATIONS — the `selected` setter and the legacy factory function's argument list.
   Called once per agent from core/html/html_element.c's declare, which owns the element-interface table
   HTMLOptionElement is a row of and the list of global names this build carries. */
void html_option_declare(JSContext *ctx);

/* §4.10.10's `attribute boolean selected` on THIS REALM's HTMLOptionElement.prototype. Handed the prototype for
   the reason html_form_install is: core/html/html_element.c owns the table, this file owns the state the member
   reads. §4.10.10's `defaultSelected` is NOT here — it is a plain `[CEReactions, Reflect="selected"]` mirror of
   the content attribute and lives in that file's reflection table, which is exactly the distinction this
   component exists to keep: one member is the attribute and the other is the state. */
void html_option_install_members(JSContext *ctx, JSValueConst option_proto);

/* Web IDL §3.7.2's legacy factory function object for `Option`, on THIS REALM's global. `proto` is this realm's
   HTMLOptionElement.prototype, which §3.7.2 makes the factory's non-configurable `prototype` property; it is a
   parameter because core/html/html_element.c's per-tag loop is what owns that object, exactly as
   html_image_install_global and html_audio_install_global are handed one. */
void html_option_install_global(JSContext *ctx, JSValueConst global, JSValueConst proto);

/* The agent's declarations, given back at teardown — core/platform.h's release column. */
void html_option_free(JSRuntime *rt);

#endif /* APICLIENT_CORE_HTML_HTML_OPTION_H */
