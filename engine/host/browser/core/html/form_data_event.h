/* THE FormDataEvent INTERFACE — HTML §4.10.22.1. See form_data_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_EVENT_H
#include "quickjs.h"

/* Declared once per AGENT, from html_form_declare — §4.10.22.1 is §4.10's interface. */
void form_data_event_init(JSContext *ctx);
/* The interface OBJECT, for one realm. Installed from html_element_install, which is where §4's interface
   objects go up and the one place that holds the global. */
void form_data_event_install(JSContext *ctx, JSValueConst global);
void form_data_event_free(JSRuntime *rt);

/* §4.10.22.4 step 7's event: a TRUSTED FormDataEvent named `formdata`, bubbling, whose `formData` attribute is
   `form_data` — the entry list's own FormData, NOT a copy, because "operations on the FormData object will
   affect form data to be submitted". OWNED, or JS_EXCEPTION. */
JSValue form_data_event_new(JSContext *ctx, JSValueConst form_data);

#endif
