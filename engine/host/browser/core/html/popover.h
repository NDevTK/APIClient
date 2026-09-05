/* THE POPOVER API — HTML §6.12 "The popover attribute". See popover.c.
 *
 * WHAT IS HERE. §6.12's per-element and per-Document state, its CHECK POPOVER VALIDITY, SHOW POPOVER, HIDE A
 * POPOVER, HIDE POPOVER STACK UNTIL, TOPMOST POPOVER ANCESTOR, TOPMOST AUTO OR HINT POPOVER, QUEUE A POPOVER
 * TOGGLE EVENT TASK and POPOVER FOCUSING STEPS, and the three IDL members `showPopover(options)`,
 * `hidePopover()` and `togglePopover(options)` those algorithms are the whole of. FOUR of them are exported and
 * the rest are not, and the line between the two lists is one question: does a section OUTSIDE §6.12 state this
 * algorithm by name? Hide a popover is reached by §6.10.2 Close watcher infrastructure as a close action; HIDE
 * POPOVERS UNTIL, TOPMOST POPOVER ANCESTOR and the POPOVER SHOWING STATE are reached by HTML §4.11.4 The dialog
 * element's `show()` and its show a modal dialog, and by FULLSCREEN §2 Model's fullscreen an element. Everything
 * those four are BUILT from — the two showing-popover lists, the hint stack parent, hide popover stack until,
 * check popover validity, show popover, the toggle task and the popover focusing steps — is named by no other
 * section and stays static, so §6.12's list algebra has exactly one reading in this build.
 *
 * THE `popover` CONTENT ATTRIBUTE'S §2.3.3 DEFINITION LIVES HERE and is exported, for the reason
 * core/html/directionality.h exports `dir`'s: core/html/html_element.c owns the TABLE of which interface a tag
 * wears and which reflections each carries, and the component that owns the attribute's ALGORITHMS owns its
 * keyword table — one fact, one place. Its three special states are three DIFFERENT states, which is what
 * §2.3.3's own machinery exists for: "the attribute's missing value default is the No Popover state, its
 * invalid value default is the Manual state, and its empty value default is the Auto state". */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_POPOVER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_POPOVER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/html/enumerated_attribute.h"

/* §6.12's four states. The No Popover state is FIRST because it is the missing value default and because
   §2.3.3's canonical-keyword question answers "no keyword" for it — `<div>.popover` is the empty string. */
enum { POPOVER_STATE_NONE = 0, POPOVER_STATE_AUTO, POPOVER_STATE_MANUAL, POPOVER_STATE_HINT };

/* §2.3.3's ATTRIBUTE DEFINITION for `popover` — the keyword table and the three special states together, which
   is what core/html/html_element.c's `popover` reflection row points at. */
extern const EnumeratedAttribute POPOVER_ATTRIBUTE;

/* §2.3.3's DETERMINE THE STATE for this element's `popover` attribute — one of the four above. */
int popover_attribute_state(const lxb_dom_element_t *el);

/* ---- §6.12's HIDE A POPOVER, as an algorithm SPEC PROSE REACHES WITHOUT A MEMBER ---------------------------
 *
 * "To hide a popover given an HTML element element, a boolean focusPreviousElement, a boolean fireEvents, a
 * boolean throwExceptions, and an HTML element or null source", 21 steps. Both IDL members state the same five
 * arguments — `hidePopover()`'s one step is "Run the hide popover algorithm given this, true, true, true, and
 * null" and `togglePopover()`'s step 5 is word for word the same — and THREE other callers state different
 * ones: §6.12's hide popover stack until steps 5 and 8 ("run the hide popover algorithm given popover,
 * focusPreviousElement, fireEvents, false, and null", and the same with fireEvents replaced by false), and
 * §6.12's show popover step 15 supplies it to §6.10.2 Close watcher infrastructure as a close action, "to hide
 * a popover given element, true, true, false, and null". So the four booleans are ARGUMENTS and not the
 * member's constants.
 *
 * IT IS A CALL AND NOT A `_run` CURSOR, AND THE REASON IS THAT §6.12 IS MUTUALLY RECURSIVE. Hide a popover step
 * 11 runs hide popover stack until, and that algorithm's steps 5 and 8 run the hide popover algorithm over each
 * popover they hide — so a hide contains a stack-until contains a hide, to a depth the page's own markup
 * decides. A cursor struct embedded in the caller's state, which is what core/html/close_watcher.h's
 * CloseWatcherRun and core/html/focus.h's focus_element_run are, cannot express that: a fixed struct cannot
 * contain itself, and those three algorithms may be one record precisely because their nesting is fixed and
 * acyclic. A registered step machine reached through step_call_run is a HEAP FRAME, which is where CLAUDE.md's
 * §C-stack puts every call, and that is what makes the recursion unbounded, preemptible and parkable at any
 * depth. It is also what lets hide popover stack until be a `_run` cursor rather than a second machine: the
 * recursion passes through the CALL, so each frame holds at most one stack-until cursor and the two shapes
 * compose instead of colliding.
 *
 * IT IS A FUNCTION OBJECT AND NOT AN EXPORTED C FUNCTION, for core/dom/observable.c's reason, which that file
 * states for §2.2.1's subscribe-to: an algorithm the standard's prose reaches WITHOUT going through the Web IDL
 * bindings must not be redirected by a page that replaces the member. §6.10.2's close action is "to hide a
 * popover given element, true, true, false, and null" and not "call this.hidePopover()", so a page assigning
 * `HTMLElement.prototype.hidePopover` must not change what a close request does. It is minted PER REALM because
 * a C function runs in the realm that DEFINED it, and this one fires `beforetoggle` at an element of that
 * realm's document.
 *
 * OWNED — the caller frees. */
#define POPOVER_HIDE_ARGC 5
/* THE CALL REQUEST BUFFER, AS A TYPE, for core/events/event_target.h's EventFireCb reason: step_call_run's
   operand shape is [this, func, args…], and a width every caller restates is a width every caller is free to be
   behind on. A caller passing this type cannot be an argument behind the algorithm, because there is no number
   at its call site to be behind with. */
#define POPOVER_HIDE_CB_SLOTS (2 + POPOVER_HIDE_ARGC)
typedef JSValue PopoverHideCb[POPOVER_HIDE_CB_SLOTS];

JSValue popover_hide_algorithm(JSContext *ctx);

/* ---- §6.12's HIDE POPOVERS UNTIL, TOPMOST POPOVER ANCESTOR, and the POPOVER SHOWING STATE ------------------
 *
 * THE THREE THINGS ANOTHER SECTION ASKS §6.12 FOR, AND NOTHING ELSE. HTML §4.11.4 The dialog element's `show()`
 * (steps 9 and 10) and its show a modal dialog (steps 5, 9, 18 and 19), and FULLSCREEN §2 Model's fullscreen an
 * element (steps 1 and 2), reach exactly these — never §6.12's SHOWING HINT POPOVER LIST, never its HINT STACK
 * PARENT, never HIDE POPOVER STACK UNTIL. Those three stay static in popover.c and the two doors below are what
 * cross this header, so §6.12's list algebra has ONE reading. A second one is not merely redundant: a popover
 * left showing over a modal dialog is exactly what hide popovers until exists to prevent, so a copy that
 * drifted would be page-visibly wrong rather than only untidy.
 *
 * HIDE POPOVERS UNTIL — "given a Document document, an HTML element or null endpoint, a boolean
 * focusPreviousElement, and a boolean fireEvents", 5 steps — RUNS THE PAGE'S CODE: its steps 2 and 5 each run
 * hide popover stack until, whose own steps 5 and 8 run hide a popover, which fires `beforetoggle` at the
 * page's listeners. So it is a REQUEST the calling machine parks on and not a call, and its state is OPAQUE and
 * heap-held for the reason core/html/html_dialog.h's DialogCloseRun is: the caller holds the pointer in its own
 * state block and names it in its own `visit`, so a fork inside one of those listeners gives each arm its own
 * half-finished walk while none of §6.12's internals crosses this header. `*slot` starts NULL, which a
 * js_mallocz'd caller state already is.
 *   Returns JS_STEP_CALL or JS_STEP_YIELD (the caller returns it), or 0 when the five steps have finished. `in`
 * is the calling machine's re-entry value and is CONSUMED. `document` is the Document WRAPPER and `endpoint` is
 * an HTML element or JS_NULL — both BORROWED, the run dups what it holds across a suspension. The caller
 * releases the state on the path that FINISHES; the visit covers the flow that is abandoned. */
typedef struct PopoverHideUntilRun PopoverHideUntilRun;

void popover_hide_popovers_until_visit(JSContext *ctx, PopoverHideUntilRun **slot, JSStepVisit *v);
void popover_hide_popovers_until_release(JSContext *ctx, PopoverHideUntilRun **slot);
int  popover_hide_popovers_until_run(JSContext *ctx, PopoverHideUntilRun **slot, JSValueConst document,
                                     JSValueConst endpoint, bool focus_previous_element, bool fire_events,
                                     JSValue in, JSValue **out_cb, int *out_argc);

/* §6.12's "To find the TOPMOST POPOVER ANCESTOR, given a Node newPopoverOrTopLayerElement, an HTML element or
   null source, and a boolean isPopover" — "they return an HTML element or null".
   IT IS EXPORTED WITH THE STANDARD'S WHOLE SIGNATURE and not with the one shape §4.11.4 passes, because a
   narrowed door is a second name for one algorithm: FULLSCREEN §2's fullscreen an element step 1 states the
   identical three arguments §4.11.4's two callers do ("given element, null, and false"). It runs NO page code —
   both showing-popover lists are derived reads over css-position-4 §3's top layer with a C predicate — so it is
   a plain call. `source` is an HTML element or JS_NULL; the answer is OWNED and JS_NULL for the standard's
   null. */
JSValue popover_topmost_popover_ancestor(JSContext *ctx, JSValueConst subject, JSValueConst source,
                                         bool is_popover);

/* CSS Scoping §4.1 "Flattening the DOM into an Element Tree"'s DESCENDANT relation — is `node` a flat tree
   descendant of `ancestor`? STRICT, as DOM's "descendant" is; a caller wanting an inclusive test compares the
   two nodes itself.
   THIS IS THE WRONG OWNER AND IT IS EXPORTED ANYWAY, which popover.c states in full at the definition. The flat
   tree is CSS Scoping's concept; the walk is here because §6.12's topmost popover ancestor is what first needed
   one, and its own two DFAILs name the component that must eventually own it. HTML §6.3.1 Modal dialogs and
   inert subtrees is the second caller ("with the exception of the subject element and its flat tree
   descendants"), and one wrong-owner door beats two right-owner copies: copies drift, and the day the flat-tree
   parent is built as its own component both callers move in that one diff.
   It runs no page code — a parent-chain walk with shadow-root reads — and it CRASHES rather than guessing at
   the two shadow edges the node tree cannot answer for. */
bool popover_flat_tree_descendant_of(JSContext *ctx, const lxb_dom_node_t *node, const lxb_dom_node_t *ancestor);

/* §6.12: "Every HTML element has a popover visibility state, initially hidden, with these potential values:
   hidden, showing." IS THIS ELEMENT IN THE POPOVER SHOWING STATE — the question §4.11.4's show a modal dialog
   asks twice, at its step 5 where the answer throws and again at its step 9 where it merely returns, because
   step 6 fired `beforetoggle` at the page in between. A slot read; it runs no page code and answers false for
   every element §6.12 has never shown. */
bool popover_element_is_showing(JSContext *ctx, JSValueConst el);

/* Declared ONCE PER AGENT (the slot keys, the three members' pool ids, hide a popover's step-def id and its
   realm slot); installed onto each realm's HTMLElement.prototype, which core/html/html_element.c owns and hands
   over — and that install is also where hide a popover's function object is minted for the realm. */
void popover_declare(JSContext *ctx);
void popover_install(JSContext *ctx, JSValueConst html_proto);
void popover_free(JSRuntime *rt);

#endif
