/* CONSTRUCTING THE ENTRY LIST — HTML §4.10.22.4. See form_entry_list.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FORM_ENTRY_LIST_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FORM_ENTRY_LIST_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* THE ALGORITHM IS A SUB-SEQUENCE, not a member: §4.10.22.4 has two callers that are two different members of
   two different standards — HTML's "submit a form" and XHR §5's `new FormData(form, submitter)` — and both are
   step machines already. So it is shaped like the other sub-sequences a machine performs across a suspension
   (event_target_fire_run, step_getprop_run): the CALLER holds the state, names it in its own `visit`, and
   releases it in its own teardown.
 *
 * IT SUSPENDS because step 7 FIRES A `formdata` EVENT at the form, which is the page's code with a live handle
 * on the list. It also YIELDS per control, because step 5 walks a list of the PAGE's size and a walk that long
 * inside one opcode is the drive-to-completion this engine has no other bound against. */
/* Steps 1/2/8's "constructing entry list" flag lives on the FORM, under a Symbol minted once per AGENT.
   Declared from html_form_declare, beside §4.10's other slot key, and released with the agent. */
void form_entry_list_declare(JSContext *ctx);
void form_entry_list_free(JSContext *ctx);
/* §4.10.21.3 STEP 2's guard, over that same flag: "if form's constructing entry list is true, then return".
   It is a separate reader rather than a second flag because the two steps are about the same fact — a form
   re-entered from its own `formdata` handler — and step 2 has to answer BEFORE step 11 fires a second `submit`
   event, which is earlier than §4.10.22.4 step 1 would notice. */
bool form_entry_list_constructing(JSContext *ctx, JSValueConst form);

typedef struct {
    uint8_t  phase;      /* this sub-sequence's own cursor — see form_entry_list.c's FEL_* */
    uint8_t  fphase;     /* the `formdata` fire request's own phase */
    uint8_t  flag_set;   /* THIS run set the form's "constructing entry list", so THIS run must clear it */
    uint32_t i;          /* step 5's cursor into `controls` */
    JSValue  form;       /* the form, held so an abandoned run can still clear the flag (owned) */
    JSValue  controls;   /* step 3's list, as a JS Array of wrappers (owned) */
    JSValue  entries;    /* steps 4-6's entry list, as the FormData it is associated with (owned) */
    JSValue  ev;         /* step 7's FormDataEvent, held across the dispatch (owned) */
    JSValue  cb[4];      /* the fire request's buffer: [this, dispatch, target, event] */
} FormEntryListRun;

/* Place the run's owned fields before anything can fail. A step state arrives js_mallocz'd, which is safe to
   RELEASE but is not JS_UNDEFINED, so a machine calls this at the stage that starts the algorithm. */
void form_entry_list_init(FormEntryListRun *r);
/* WHAT THIS RUN OWNS — the caller's `visit` forwards to it, so a fork mid-construction gives each arm its own
   entry list rather than two flows one. */
void form_entry_list_visit(JSContext *ctx, FormEntryListRun *r, JSStepVisit *v);
/* The caller's teardown forwards to it. An abandoned run CLEARS the form's constructing-entry-list flag, which
   step 8 would otherwise have done: a flow dropped inside step 5 must not leave the form unable to submit. */
void form_entry_list_release(JSContext *ctx, FormEntryListRun *r);

/* §4.10.22.4 itself. `encoding` is the encoding NAME step 5.9's `_charset_` entry carries — "UTF-8" for the
   default the algorithm declares. Returns JS_STEP_CALL or JS_STEP_YIELD (the caller returns it), -1 with the
   throw live, or 0 once `*pout` holds step 9's answer: a FormData over a CLONE of the entry list, or JS_NULL
   when step 1 refused because the form was already constructing one. */
int form_entry_list_run(JSContext *ctx, FormEntryListRun *r, JSValueConst form, JSValueConst submitter,
                        const char *encoding, JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc);

#endif
