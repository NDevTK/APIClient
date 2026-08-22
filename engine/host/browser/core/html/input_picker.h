/* §4.10.5.4's `showPicker()` — the one member of HTMLInputElement that OPENS A PICKER, and the only caller
 * §4.10.5.1.17's file dialog has. See input_picker.c.
 *
 * ONE PROBLEM: what happens when a page asks the user agent to show a control's picker. That is two algorithms
 * — the method's five steps and the "show the picker, if applicable" they end in — and they are here together
 * because the second is the first's whole body and the standard shares it with the File Upload state's INPUT
 * ACTIVATION BEHAVIOR ("the input activation behavior for such an element element is to show the picker, if
 * applicable, for element"). It is NOT in input_value.c: that file owns what an input's VALUE is, and a picker
 * is what a user does; the two meet at §4.10.5.1.17's update the file selection, which input_value.c exports
 * and this file calls.
 *
 * WHY IT IS THE COMPONENT THE ACTIVATION STATE WAS BUILT FOR. Every path through both algorithms is gated on
 * §6.4.1's TRANSIENT ACTIVATION, and the third of them CONSUMES it — so a `showPicker()` that could only ever
 * throw NotAllowedError left user_activation.c's whole processing model with no caller, and left the picker
 * world unexplored. The activation state is unknown external input (user_activation.h says why), so the gate
 * FORKS: one flow throws and runs the page's fallback path, its sibling opens the picker, spends the page's
 * activation and delivers the files. Both are code a real bundle ships and only one of them is what a browser
 * with a user in front of it does. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_INPUT_PICKER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_INPUT_PICKER_H

#include "quickjs.h"

/* Declared once per AGENT (from html_form_declare, beside §4.10's other members) and installed on the
   HTMLInputElement.prototype §4.10 owns. */
void input_picker_declare(JSContext *ctx);
void input_picker_install(JSContext *ctx, JSValueConst input_proto);
void input_picker_free(void);

#endif
