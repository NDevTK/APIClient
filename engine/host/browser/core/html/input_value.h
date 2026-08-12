/* AN INPUT'S VALUE — HTML §4.10.5.1's VALUE SANITIZATION ALGORITHM and §4.10.5.4's four VALUE MODES.
 *
 * ONE PROBLEM: what `input.value` IS. It is not the `value` content attribute and it is not the slot a script
 * last wrote — it is the element's VALUE, a piece of state whose relationship to that attribute is decided by
 * the DIRTY VALUE FLAG and whose CONTENT is decided by the `type` attribute's state through an algorithm the
 * standard runs at four named moments. Every reader of a control's value — §4.10.21's constraint validation,
 * §4.10.22.4's entry list, and the IDL attribute itself — reads it through here, so they are right or wrong
 * TOGETHER: `<input type=url value=" http://x ">` reported a typeMismatch a browser does not, because a browser
 * stripped the leading and trailing whitespace before anything looked at the value. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_INPUT_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_INPUT_VALUE_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/html/html_form.h"

/* §4.10.5.4's MODES of the `value` IDL attribute. Each state of the `type` attribute names exactly one, and the
   getter AND the setter differ per mode — reading the mode wrong is a different bug from sanitizing wrong: a
   `type=hidden` assignment writes the CONTENT ATTRIBUTE (default mode) and a `type=text` one does not (value
   mode), and neither one is "store it in a slot". */
typedef enum {
    INPUT_VALUE_MODE_VALUE = 0,   /* the element's own value; the setter sets the dirty value flag and sanitizes */
    INPUT_VALUE_MODE_DEFAULT,     /* the `value` content attribute, or the empty string */
    INPUT_VALUE_MODE_DEFAULT_ON,  /* the `value` content attribute, or the string "on" */
    INPUT_VALUE_MODE_FILENAME,    /* "C:\fakepath\" + the first selected file, and a setter that mostly throws */
} HtmlInputValueMode;

/* WHICH MODE a state puts the `value` IDL attribute in — §4.10.5.1.1 through §4.10.5.1.21's own sentence, in
   one place. The three date-and-time states and Number/Range/Color are mode value like the text states;
   Checkbox and Radio Button are default/on; File Upload is filename; Hidden and the four button states are
   default. Nothing else asks this question twice. */
HtmlInputValueMode input_value_mode(HtmlInputState st);

/* §4.10.5.4's `value` IDL attribute, per mode. `wrap` is an `input` element's wrapper — every other control's
   value belongs to its own section (§4.10.11's textarea, §4.10.10's option) and does not come through here.
   The getter is OWNED; the setter answers JS_UNDEFINED, or JS_EXCEPTION for the filename mode's
   "InvalidStateError". A CONCOLIC survives both: the value slot and the attribute taint shadow hold the value
   itself, so `input.value = location.hash` reaches §4.10.22.4's entry list as the source it came from. */
JSValue input_value_get(JSContext *ctx, JSValueConst wrap);
JSValue input_value_set(JSContext *ctx, JSValueConst wrap, JSValueConst val);

#endif
