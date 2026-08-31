/* HTML §4.10.14 "The meter element" — the SIX POINTS ON THE GAUGE, and the list of the element's labels.
 *
 * ONE ALGORITHM, SIX ANSWERS, AND THE ORDER IS THE ALGORITHM. §4.10.14: "User agents must then use all these
 * numbers to obtain values for six points on the gauge, as follows. (The order in which these are evaluated is
 * important, as some of the values refer to earlier ones.)" The maximum value is clamped up to the minimum, the
 * actual value is clamped into the pair, the low boundary defaults to the minimum, the high boundary defaults
 * to the maximum and is clamped up to the LOW BOUNDARY rather than to the minimum, and the optimum point
 * defaults to the midpoint of the range. So `<meter min=5 max=1>` has a maximum of 5, `<meter value=9 max=2>`
 * an actual value of 2, and `<meter low=8 high=3 max=10>` a high boundary of 8 — none of which is reachable by
 * reading six attributes independently.
 *
 * THIS IS WHY THE TWO ROWS THAT USED TO STAND IN core/html/html_element.c's TABLE WERE WRONG RATHER THAN
 * PARTIAL. `min` and `max` were declared REFLECT_STRING, so `meter.min` answered the ATTRIBUTE'S BYTES — the
 * string "5", or "" for an absent attribute — where §4.10.14's IDL declares `attribute double min` and its
 * getter steps say "return this's minimum value", which is the number 0 for that same absent attribute.
 * `+meter.min` and `meter.min === 0` went two different ways from one element, and no member-presence audit can
 * see it: the name was there and the value was another type. The six are a component because their answers are
 * an ALGORITHM over all six attributes and not a mirror of any one of them, which is exactly what
 * core/dom/element.h's reflection enum says a member whose IDL carries `[ReflectSetter]` is.
 *
 * THE SETTERS ARE STILL §2.6.1'S, AND THAT ASYMMETRY IS `[ReflectSetter]`'s whole meaning: `meter.min = 5`
 * writes the `min` content attribute and nothing else, so `<meter min=5 max=1>` keeps a `max` attribute of 1
 * while `meter.max` reads 5. Asked of core/dom/element.h, which owns §2.6.1.
 *
 * §4.10.14's "UA requirements for regions of the gauge" and "for showing the gauge" are the only parts of the
 * section a display device is needed for, and neither is observable through this interface — every number above
 * is defined without one. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_METER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_METER_H

#include "quickjs.h"

/* The AGENT's half: §4.10.14's six `[ReflectSetter]` setters, declared once per runtime. */
void html_meter_declare(JSContext *ctx);
/* §4.10.14's `value`, `min`, `max`, `low`, `high`, `optimum` and `labels` on HTMLMeterElement.prototype. Handed
   the prototype by core/html/html_element.c, which owns the table of which interface a tag wears. */
void html_meter_install(JSContext *ctx, JSValueConst proto);

#endif
