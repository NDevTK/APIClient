/* THE TEXT CONTROL SELECTION — HTML §4.10.20 "APIs for the text control selections". See
 * text_control_selection.c.
 *
 * ONE PROBLEM: WHERE THE SELECTION IS IN A TEXT CONTROL, AND WHAT MOVES IT. HTML §4.10.20 opens by saying it is one
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
#include "core/html/html_form.h"

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

/* HTML §4.10.5 The input element's TYPE CHANGE STEPS 7-9, as ONE operation, over the state the control is
 * LEAVING — the state it is arriving in is on the element, because §4.9's attribute change steps fire after
 * the write.
 *
 *   7. "Let previouslySelectable be true if setRangeText() previously applied to the element, and false
 *      otherwise."
 *   8. "Let nowSelectable be true if setRangeText() now applies to the element, and false otherwise."
 *   9. "If previouslySelectable is false and nowSelectable is true, set the element's text entry cursor
 *      position to the beginning of the text control, and set its selection direction to \"none\"."
 *
 * WHY THE THREE STEPS ARE ONE ENTRY AND NOT A `move_to_start` PRIMITIVE ITS CALLER GUARDS. Steps 7 and 8 ask
 * whether §4.10.20's `setRangeText()` APPLIED IN A STATE, which is this file's per-state bookkeeping and
 * nobody else's; handing that question out as a second exported predicate would put a second public spelling
 * of applicability beside text_control_selection_applies, which is the shape two answers to one question
 * drift from. And the move itself has EXACTLY ONE call site in the whole standard — step 9 is the only place
 * in HTML that moves a text entry cursor to the beginning of a text control — so splitting it from its own
 * condition buys no reuse and costs a second file that has to know `setRangeText()` is an offset member.
 * Contrast text_control_selection_move_to_end, which is exported as a bare move BECAUSE it genuinely has two
 * callers writing one sentence twice.
 *
 * IT TAKES THE ELEMENT AND NOT A WRAPPER, unlike the two operations above, and the difference is exactly what
 * each one needs: those two move the cursor to the END of the relevant value, which is read through the `value`
 * IDL getter and therefore through a wrapper, while the BEGINNING is 0 and needs no value at all. It matters
 * because §4.9's attribute change steps fire during an `innerHTML` PARSE as well as for `i.type = "radio"`, so
 * a wrapper minted here would be one JS object per parsed `<input type=...>` for a question that never reads it
 * — and text_control_selection_applies already takes a node, so this is that spelling and not a third one.
 *
 * IT FIRES NO `select` EVENT, for the same reason move_to_end does not: step 9 moves the state directly and is
 * not one of set the selection range's callers. */
void text_control_selection_type_changed(JSContext *ctx, lxb_dom_element_t *el, HtmlInputState was);

/* Whether §4.10.20's five offset members apply to an `input` in this state — the standard's own per-state
   bookkeeping lists, read as ONE predicate so the twenty-one states are answered from one place. Exported
   because HTML §4.10.5.4's value setter has to know whether the element HAS a text entry cursor position before it
   runs its step 5 ("...and the element has a text entry cursor position"), and that is this question. */
bool text_control_selection_applies(const lxb_dom_node_t *n);

#endif
