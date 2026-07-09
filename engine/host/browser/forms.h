/* HTML form submission -> @H endpoint.
 *
 * form.submit()/requestSubmit() collects the form's named controls into an endpoint (method from the form,
 * url from `action` resolved against the origin, params in the query or a urlencoded body) and records it —
 * exactly like a real browser firing the action request, so form-driven endpoints are learned. A control
 * whose value was JS-set to a tainted source carries its concolic example (via the attr shadow). */
#ifndef ENGINE_HOST_FORMS_H
#define ENGINE_HOST_FORMS_H

#include "quickjs.h"

JSValue js_form_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

#endif
