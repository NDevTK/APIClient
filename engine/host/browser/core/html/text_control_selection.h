/* THE TEXT CONTROL SELECTION — HTML §4.10.20 "APIs for the text control selections". See
 * text_control_selection.c.
 *
 * ONE PROBLEM: WHERE THE SELECTION IS IN A TEXT CONTROL, AND WHAT MOVES IT. §4.10.20 opens by saying it is one
 * problem and not two — "The input and textarea elements define several attributes and methods for handling
 * their selection. THEIR SHARED ALGORITHMS ARE DEFINED HERE" — so the six members go in one component reached
 * from two prototypes, exactly as §4.10.21's constraint validation does. Written inside either element's own
 * file it would be that element's private opinion of where a cursor is, and `set the selection range` is the
 * one algorithm every other member in the section ends in.
 *
 * THE STATE IS PER ELEMENT AND PER FLOW. "All input elements to which these APIs apply, and all textarea
 * elements, have either a selection or a text entry cursor position AT ALL TIMES (even for elements that are
 * not being rendered)" — so this is real modelled state and not a rendering artifact, which is why a headless
 * agent owes the page a real answer here rather than a shrug. It lives in the per-flow property shadow the
 * control's value already uses (solver/attr_shadow.h through solver/dom_cow.h), so a forked arm that moved the
 * cursor and its sibling that did not are two timelines, and a parked flow resumes with the offsets it had.
 *
 * WHY THE GAP WAS SILENT RATHER THAN ABSENT, which is what made it worth taking. A member missing from a
 * prototype does not throw on ASSIGNMENT: `el.selectionStart = 0` on a wrapper with no such accessor creates an
 * ordinary own property and reads back the number that was written, so an editor's `a.selectionStart !=
 * a.selectionEnd` answered a plausible false forever and nothing anywhere said so. That is CLAUDE.md
 * §NO STUBS' "a plausible datum is indistinguishable from a measurement" arriving through an ABSENCE instead of
 * through a stub, and it is the reason this section is not merely missing coverage — it was answering wrongly.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TEXT_CONTROL_SELECTION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TEXT_CONTROL_SELECTION_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared once per AGENT (the member ids and the `select` task's machine), installed per realm on the two
   interfaces that DECLARE §4.10.20's members. Both are reached from §4.10's declaration point in html_form.c,
   because that is where these prototypes arrive. */
void text_control_selection_declare(JSContext *ctx);
void text_control_selection_install(JSContext *ctx, JSValueConst input_proto, JSValueConst textarea_proto);
void text_control_selection_free(JSRuntime *rt);

/* §4.10.20's "WHENEVER THE RELEVANT VALUE CHANGES for an element to which these APIs apply, run these steps" —
 * the CLAMP: an offset now past the end of the relevant value is moved to the end.
 *
 * IT IS NOT THE SAME OPERATION AS THE ONE BELOW and the two are called from the same places, which is exactly
 * why both are named here. This one runs on EVERY change of the relevant value and preserves a selection that
 * still fits; the value setters additionally run the other one, and only when the value actually CHANGED.
 * Safe on a control that has no selection state yet — an untouched control's cursor is at 0 and clamps to 0. */
void text_control_selection_value_changed(JSContext *ctx, JSValueConst wrap);

/* THE LAST STEP OF BOTH VALUE SETTERS: "move the text entry cursor position to the end of the text control,
 * unselecting any selected text and resetting the selection direction to \"none\"" — §4.10.5.4's value-mode
 * setter step 5 and §4.10.11's `value` setter step 4, which are the same sentence written twice because the
 * two controls keep their values in different places and their cursors in the same one.
 * IT FIRES NO `select` EVENT: it is a direct move of the state and not §4.10.20's set-the-selection-range, and
 * only that algorithm queues the task. */
void text_control_selection_move_to_end(JSContext *ctx, JSValueConst wrap);

/* Whether §4.10.20's five offset members apply to an `input` in this state — the standard's own per-state
   bookkeeping lists, read as ONE predicate so the twenty-one states are answered from one place. Exported
   because §4.10.5.4's value setter has to know whether the element HAS a text entry cursor position before it
   runs its step 5 ("...and the element has a text entry cursor position"), and that is this question. */
bool text_control_selection_applies(const lxb_dom_node_t *n);

#endif
