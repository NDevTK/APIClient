/* FORMS — HTML §4.10: a form's controls, their VALUE state, and submission. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_H
#include "quickjs.h"

/* Install §4.10's members on the interfaces that DECLARE them. The html layer owns the per-tag prototypes and
   hands them over; this file owns the algorithms. */
void html_form_install(JSContext *ctx, JSValueConst form_proto, JSValueConst input_proto,
                       JSValueConst textarea_proto, JSValueConst option_proto);
/* `document.forms` — a Document member, so document.c installs it on its prototype. */

#endif
