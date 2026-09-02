/* THE POPOVER API — HTML §6.12 "The popover attribute". See popover.c.
 *
 * WHAT IS HERE. §6.12's per-element and per-Document state, its CHECK POPOVER VALIDITY, SHOW POPOVER, HIDE A
 * POPOVER, HIDE POPOVER STACK UNTIL, TOPMOST POPOVER ANCESTOR, TOPMOST AUTO OR HINT POPOVER, QUEUE A POPOVER
 * TOGGLE EVENT TASK and POPOVER FOCUSING STEPS, and the three IDL members `showPopover(options)`,
 * `hidePopover()` and `togglePopover(options)` those algorithms are the whole of. Only hide a popover is
 * exported, and the reason is stated at it: it is the one §6.12 algorithm another SECTION reaches.
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

/* Declared ONCE PER AGENT (the slot keys, the three members' pool ids, hide a popover's step-def id and its
   realm slot); installed onto each realm's HTMLElement.prototype, which core/html/html_element.c owns and hands
   over — and that install is also where hide a popover's function object is minted for the realm. */
void popover_declare(JSContext *ctx);
void popover_install(JSContext *ctx, JSValueConst html_proto);
void popover_free(JSRuntime *rt);

#endif
