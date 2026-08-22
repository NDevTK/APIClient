/* THE SubmitEvent INTERFACE — HTML §4.10.22.10. See submit_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_SUBMIT_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_SUBMIT_EVENT_H

#include "quickjs.h"

/* Declared once per AGENT, from html_form_declare — §4.10.22.10 is §4.10's interface, and §4.10 has one
   declaration point. The per-realm half declares ITSELF into realm.h's one list, so every realm this agent
   builds has SubmitEvent.prototype and the interface object the IDL's [Exposed=Window] puts on its global. */
void submit_event_init(JSContext *ctx);
void submit_event_free(JSRuntime *rt);

/* §4.10.22.3 step 5.6's event: "fire an event named submit at form using SubmitEvent, with the submitter
   attribute initialized to submitterButton, the bubbles attribute initialized to true, and the cancelable
   attribute initialized to true". TRUSTED — the user agent fired it.
   `submitter` is step 5.5's submitterButton: the submit BUTTON, or JS_NULL when the submission named none.
   BORROWED. Returns a new owned SubmitEvent, or JS_EXCEPTION. */
JSValue submit_event_new(JSContext *ctx, JSValueConst submitter);

#endif
