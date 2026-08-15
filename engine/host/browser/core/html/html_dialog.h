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
void html_dialog_free(JSContext *ctx);

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

#endif
