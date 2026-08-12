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

/* §4.10.5.4's `files`, declared once per AGENT and installed on HTMLInputElement.prototype — the same three
   calls §4.10.21's constraint validation makes, from the same places, because the member goes on a prototype
   §4.10 owns and the algorithm behind it is this file's. */
void input_value_declare(JSContext *ctx);
void input_value_install(JSContext *ctx, JSValueConst input_proto);
void input_value_free(void);

/* ---- §4.10.5.1.17's LIST OF SELECTED FILES ------------------------------------------------------------------
 *
 * "The input element represents a list of selected files, each file consisting of a filename, a file type, and
 * a file body." It is per-element, per-flow state held as the File API §5 FileList that IS it — see
 * input_value.c. Every one of these asserts that the control is in the File Upload state, because the list
 * belongs to that state and to no other; a caller that is not sure asks html_form_input_state first. */

/* §4.10.5.4's `files` getter as its algorithm: the element's FileList, or JS_NULL when the member does not
   apply. OWNED. It MINTS the empty list on first ask and stores it, because "the same object must be returned
   until the list of selected files changes". */
JSValue input_files_get(JSContext *ctx, JSValueConst wrap);
/* THE TWO READS THAT MINT NOTHING — for §4.10.21.1's "the list of selected files is empty" and §4.10.22.4 step
   5.8/5.9's walk, neither of which may create state by asking. `item` is OWNED, JS_UNDEFINED past the end. */
uint32_t input_files_count(JSContext *ctx, JSValueConst wrap);
JSValue  input_files_item(JSContext *ctx, JSValueConst wrap, uint32_t i);
/* "EMPTY the list of selected files" — §4.10.5.4's filename-mode setter, and §4.10.5.1's reset algorithm. A
   no-op when the list is already empty, which keeps the FileList's identity stable across it. */
void input_files_clear(JSContext *ctx, JSValueConst wrap);
/* §4.10.5.1.17's UPDATE THE FILE SELECTION: replace the list with `files` (a FileList), then queue the element
   task that fires `input` and then `change`. This is what a user's selection DOES, and the only way the list
   grows. */
void input_files_update(JSContext *ctx, JSValueConst wrap, JSValueConst files);
/* THE ENGINE-SIDE SELECTION — the entry that stands in for the user at this control's picker: ask
   core/file/file_device.c what this control's `accept` admits (at most one file unless `multiple`), and update
   the file selection with the answer. Answers how many files were selected; 0 leaves the control untouched,
   which is the prompt nobody chose in. A control that is not mutable selects nothing. */
uint32_t input_files_pick(JSContext *ctx, JSValueConst wrap);

#endif
