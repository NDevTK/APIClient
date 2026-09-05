/* The dialog element — HTML §4.11.4. See html_dialog.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_DIALOG_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_DIALOG_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* Declared once per AGENT (the two slot keys and the toggle task's step definition); installed per REALM on
   HTMLDialogElement.prototype, which core/html/html_element.c owns the table of and therefore hands over. */
void html_dialog_declare(JSContext *ctx);
void html_dialog_install(JSContext *ctx, JSValueConst dialog_proto);
void html_dialog_free(JSRuntime *rt);

/* IS THIS NODE A `dialog` ELEMENT — §4.10.22.3 step 11.1's "form does not have an ancestor dialog element" and
   step 11.2's nearest one are both this question, asked of a NODE because the walk that asks it is over the
   tree rather than over wrappers. */
bool html_dialog_is_dialog(const lxb_dom_node_t *n);

/* §4.11.4's CLOSE THE DIALOG — "when a dialog element subject is to be closed, with null or a string result and
 * an Element or null source". It is the whole of what a `method=dialog` submission does instead of making a
 * request, and it is a SUB-SEQUENCE rather than a call for the reason §4.10.21.2's is: step 2 fires
 * `beforetoggle` at the dialog, which is the page's code, so the calling machine has to be able to park across
 * it and resume at step 3.
 *
 * The state is OPAQUE and heap-allocated, held by the caller in its own state block and named in its own
 * `visit` — so a fork inside a `beforetoggle` handler gives each arm its own half-finished close. `*slot`
 * starts NULL, which a js_mallocz'd caller state already is.
 *
 * `result` is the spec's "null or a string" and `source` its "Element or null"; both are BORROWED for the
 * length of the call — the run dups what it holds across the suspension. */
typedef struct DialogCloseRun DialogCloseRun;

void html_dialog_close_visit(JSContext *ctx, DialogCloseRun **slot, JSStepVisit *v);
void html_dialog_close_release(JSContext *ctx, DialogCloseRun **slot);

/* Returns JS_STEP_CALL (the caller returns it), -1 with the throw live, or 0 when the dialog is closed. `in` is
   the calling machine's `cb_result` and is CONSUMED — forwarded to the fire request when one is in flight and
   released otherwise, so the caller never frees it. */
int html_dialog_close_run(JSContext *ctx, DialogCloseRun **slot, JSValueConst subject, JSValueConst result,
                          JSValueConst source, JSValue in, JSValue **out_cb, int *out_argc);


/* §4.11.4's DIALOG FOCUSING STEPS, 10 steps — "The dialog focusing steps, given a dialog element subject, are
 * as follows". Run at `show()` step 11, at show a modal dialog step 20, and at HTML §6.12 The popover
 * attribute's POPOVER FOCUSING STEPS step 2, which is the caller this door is exported for: "If subject is a
 * dialog element, then run the dialog focusing steps given subject and return."
 *
 * TWO OF THE TEN ARE REQUESTS, so this is a cursor and not a call: step 1's §6.6.6 allow focus steps FORKS
 * (its second clause is §6.4.1's transient activation, unknown external state), and step 6's §6.6.4 focusing
 * steps fire `blur`, `focusout`, `focus` and `focusin` at the page's listeners. The state is OPAQUE and
 * heap-held, the same shape and the same reason as DialogCloseRun above: the caller holds the pointer in its
 * own state block and names it in its own `visit`, so a fork inside one of those listeners gives each arm its
 * own half-finished run. `*slot` starts NULL, which a js_mallocz'd caller state already is.
 *
 * `hdr` is the CALLING machine's header, which step 1's fork is taken at. `subject` is BORROWED — the run dups
 * it across the suspension. Returns JS_STEP_FORK or JS_STEP_CALL (the caller returns it), or 0 when the ten
 * steps have finished; `in` is the calling machine's re-entry value and is CONSUMED. The caller releases the
 * state on the path that FINISHES; the visit covers the flow that is abandoned. */
typedef struct DialogFocusingRun DialogFocusingRun;

void html_dialog_focusing_visit(JSContext *ctx, DialogFocusingRun **slot, JSStepVisit *v);
void html_dialog_focusing_release(JSContext *ctx, DialogFocusingRun **slot);
int  html_dialog_focusing_run(JSContext *ctx, JSStepHdr *hdr, DialogFocusingRun **slot, JSValueConst subject,
                              JSValue in, JSValue **out_cb, int *out_argc);

/* §4.11.4's "Each dialog element has an is modal boolean, initially false." Read by HTML §6.12 The popover
   attribute's check popover validity step 3, whose fourth disjunct refuses a `dialog` element whose is modal is
   true. It is a QUESTION about a slot and runs no page code, so it is a plain predicate; false for anything
   that is not a `dialog`, which is the answer that disjunct's own conjunct already gives. */
bool html_dialog_is_modal(JSContext *ctx, JSValueConst el);

/* §4.11.4's "Each HTML element has a previously focused element, which is null or an element, and it is
 * initially null."
 *
 * IT IS DECLARED BY §4.11.4 AND WRITTEN BY TWO SECTIONS, WHICH IS WHY IT LIVES HERE AND IS EXPORTED. The
 * standard's own sentence names both writers in one breath: "When showModal() and show() are called, this
 * element is set to the currently focused element before running the dialog focusing steps. Elements with the
 * popover attribute set this element to the currently focused element during the show popover algorithm." So it
 * is ONE field over two algorithms, and a second copy in whichever file happens to write it first is the
 * one-fact-two-answers defect with a browser-visible consequence: `<dialog open popover>` shown through
 * `showPopover()` and then closed by a `method=dialog` submission must restore focus at close the dialog step
 * 12, and a §6.12-private copy answers null there.
 *
 * `el` is any HTML element's wrapper. The read is OWNED and answers JS_NULL for the standard's null; the write
 * takes JS_NULL for "set to null" and never leaves a value behind for a later read to find. */
JSValue html_dialog_previously_focused_element(JSContext *ctx, JSValueConst el);
void html_dialog_set_previously_focused_element(JSContext *ctx, JSValueConst el, JSValueConst v);

/* THE TWO FIELDS THE CLOSE ACTION FOR A DIALOG READS, and it reads them at the moment the action RUNS rather
   than at the moment the watcher was established: §4.11.4's set the dialog close watcher step 3 supplies
   "closeAction being to close the dialog given dialog, dialog's request close return value, and dialog's
   request close source element", and `requestClose()` writes both between the establish and the run. Both are
   OWNED and answer JS_NULL for the standard's null. */
JSValue html_dialog_request_close_return_value(JSContext *ctx, JSValueConst dialog);
JSValue html_dialog_request_close_source_element(JSContext *ctx, JSValueConst dialog);

/* THE GET ENABLED STATE §4.11.4's set the dialog close watcher step 3 supplies, whole: "getEnabledState being
   to return true if dialog's enable close watcher for request close is true or dialog's computed closed-by
   state is not None; otherwise false." §6.10.2 declares that algorithm "can never throw an exception", so it is
   a predicate and not a request — every one of its questions is a slot read or a content-attribute read. */
bool html_dialog_close_watcher_enabled(JSContext *ctx, JSValueConst dialog);

/* HTML §6.3.1 Modal dialogs and inert subtrees: "A Document document is BLOCKED BY A MODAL DIALOG subject if
 * subject is the topmost dialog element in document's top layer."
 *
 * A DERIVED PREDICATE WITH NO STORAGE ANYWHERE, which is why §4.11.4's show a modal dialog step 14 — "Set
 * subject's node document to be blocked by the modal dialog subject" — writes no field: it is PERFORMED by
 * step 15's add to the top layer, and step 14's site carries a two-sided assert instead. A stored bit would be
 * a second answer to a question css-position-4 §3's top layer already answers, and the two would part company
 * the moment §4.11.4's removing steps take the element out of the layer or a rendering update processes a
 * pending removal.
 * It lives in §4.11.4's component because `dialog` is §4.11.4's element, and it reads §3's ORDER through
 * core/css/top_layer.h's own walk rather than by indexing the set. Runs no page code. The answer is the
 * SUBJECT — OWNED, and JS_NULL for a document no modal dialog blocks. */
JSValue html_dialog_blocked_by_modal_dialog(JSContext *ctx, JSValueConst document);

/* The rest of §6.3.1's sentence, as the question HTML §6.3's INERT asks of one node: "While document is so
 * blocked, every node that is connected to document, with the exception of the subject element and its flat
 * tree descendants, must become inert."
 * IT IS HERE AND NOT AT core/html/focus.c's inert walk for the reason the predicate above is: both halves of
 * the sentence are about a `dialog`, and answering the second where the first is answered is what keeps the
 * exception and the blocking from being read out of two places. The flat-tree half is CSS Scoping §4.1's and is
 * routed to the ONE walk this build has, which core/html/popover.h explains and which CRASHES rather than
 * guessing at the shadow edges the node tree cannot answer for. Runs no page code. */
bool html_dialog_node_is_blocked_by_modal_dialog(JSContext *ctx, const lxb_dom_node_t *n);

/* §4.11.4's ATTRIBUTE CHANGE STEPS, 6 steps, "given element, localName, oldValue, value, and namespace … used
   for dialog elements" — registered on core/dom/element.c's element_attr_changed, which is the one place every
   spelling of a write reaches. BOTH VALUES CROSS because steps 3 and 6 turn on ADDED and REMOVED rather than on
   the value: `open=""` overwritten with `open="x"` is neither, and this is the hook that establishes and
   destroys the dialog's close watcher. `val` is NULL for a removal and `old_val` NULL for an absent attribute,
   which are the standard's two nulls. */
void html_dialog_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *old_val, const char *val);

/* §4.11.4's "dialog HTML element insertion steps, given insertedNode", 2 steps. They have no `<dfn>` of their
   own in the standard — they hang off the shared HTML element insertion steps link — and they are what makes a
   PARSED `<dialog open>` dismissable: the parser sets the attribute before insertion, so the attribute change
   steps see a disconnected node and return. Reached from core/dom/element.c's §4.2.3 insertion-steps drain.
   They run no page code, which is what §4.2.3 requires of every member of that list. */
void html_dialog_insertion_steps(JSContext *ctx, JSValueConst el);

/* THE SAME TWO STEPS FOR THE TREE A LOAD'S PARSE BUILT, which reaches none of DOM §4.2.3's mutation
   algorithms: HTML §13.2.6 Tree construction writes through solver/dom_cow.c's own lexbor callback table and
   never fires the insertion hook, so every component whose insertion steps matter to the markup has a walk of
   this shape and core/dom/element.c's own residual names the family. Without it a `<dialog open>` a page SHIPS
   has no close watcher while one a script inserts does, and §4.11.4's request to close the dialog asserts at
   its step 3 that an open, connected, fully active dialog has one — so the absence would be an abort on
   ordinary markup rather than a quiet gap. SHADOW-INCLUDING, because a `<template shadowrootmode>`'s contents
   are in a shadow root by the time this runs. */
void html_dialog_parsed(JSContext *ctx, lxb_dom_node_t *root);

/* §4.11.4's "dialog HTML element removing steps, given removedNode, isSubtreeRoot, and oldAncestor", 3 steps.
   Neither of the last two arguments is read by any of the three, so only the node crosses. Reached from
   core/dom/element.c's §4.2.3 removing-steps drain, beside §4.8.5's; `el` is the removed element's wrapper. */
void html_dialog_removing_steps(JSContext *ctx, JSValueConst el);

#endif
