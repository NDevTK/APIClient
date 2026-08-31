/* HTML §4.10.13 "The progress element" — the three numbers a `<progress>` answers with, and the list of its
 * labels.
 *
 * WHY IT IS A COMPONENT AND NOT FOUR ROWS OF A REFLECTION TABLE. §4.10.13 declares
 * `[CEReactions, ReflectSetter] attribute double value` and `readonly attribute double position`, and neither
 * getter is a mirror of an attribute: `value` is "0 if this is an indeterminate progress bar; otherwise this's
 * current value", `position` is "−1" or "the result of dividing the current value by the maximum value", and
 * BOTH are defined over "the maximum value of the progress bar", which is what §4.10.13's `max` answers. One
 * number, three members — so the three live together, over one implementation of it, which is exactly what
 * core/dom/element.h's reflection enum says a `[ReflectSetter]` member is.
 *
 * `max` IS THE ONE MEMBER OF THIS FILE THAT IS §2.6.1'S DOUBLE MODEL IN BOTH DIRECTIONS —
 * `[CEReactions, ReflectPositive, ReflectDefault=1.0]` — and its answer IS the maximum value of the progress
 * bar: §2.6.1's getter returns the parsed attribute when it is greater than zero and 1.0 otherwise, which is
 * word for word §4.10.13's own "otherwise … the maximum value of the progress bar is 1.0". It is asked of
 * core/dom/element.h rather than reimplemented, so the two cannot drift apart.
 *
 * INDETERMINATE IS THE PRESENCE OF AN ATTRIBUTE, NEVER ITS VALUE. §4.10.13: "If the value attribute is omitted,
 * then the progress bar is an indeterminate progress bar. Otherwise, it is a determinate progress bar." So
 * `<progress value="x">` is DETERMINATE with a value of zero (the parse failed, and the value of the progress
 * bar is then zero) while `<progress>` is indeterminate and answers `position` −1 — two different answers that
 * a test written over the parsed number alone gives one of.
 *
 * THERE IS NO RENDERING HERE AND NOTHING IS MISSING BECAUSE OF IT. §4.10.13's "UA requirements for showing the
 * progress bar" is the only part of the section a display device is needed for; every number above is defined
 * without one, which is what CLAUDE.md §Headless means by modelling the state. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_PROGRESS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_PROGRESS_H

#include "quickjs.h"

/* The AGENT's half: §4.10.13's one reflecting setter (`value`), declared once per runtime. */
void html_progress_declare(JSContext *ctx);
/* §4.10.13's `value`, `max`, `position` and `labels` on HTMLProgressElement.prototype. Handed the prototype by
   core/html/html_element.c, which owns the table of which interface a tag wears, for the same reason §4.12.1's
   `async` is: none of the four is a plain reflection and this file owns the algorithms. */
void html_progress_install(JSContext *ctx, JSValueConst proto);

#endif
