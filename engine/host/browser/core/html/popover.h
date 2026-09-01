/* THE POPOVER API — HTML §6.12 "The popover attribute". See popover.c.
 *
 * WHAT IS HERE. §6.12's per-element and per-Document state, its CHECK POPOVER VALIDITY, SHOW POPOVER, HIDE A
 * POPOVER, QUEUE A POPOVER TOGGLE EVENT TASK and POPOVER FOCUSING STEPS, and the three IDL members
 * `showPopover(options)`, `hidePopover()` and `togglePopover(options)` those algorithms are the whole of.
 *
 * THE `popover` CONTENT ATTRIBUTE'S §2.3.3 DEFINITION LIVES HERE and is exported, for the reason
 * core/html/directionality.h exports `dir`'s: core/html/html_element.c owns the TABLE of which interface a tag
 * wears and which reflections each carries, and the component that owns the attribute's ALGORITHMS owns its
 * keyword table — one fact, one place. Its three special states are three DIFFERENT states, which is what
 * §2.3.3's own machinery exists for: "the attribute's missing value default is the No Popover state, its
 * invalid value default is the Manual state, and its empty value default is the Auto state". */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_POPOVER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_POPOVER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/html/enumerated_attribute.h"

/* §6.12's four states. The No Popover state is FIRST because it is the missing value default and because
   §2.3.3's canonical-keyword question answers "no keyword" for it — `<div>.popover` is the empty string. */
enum { POPOVER_STATE_NONE = 0, POPOVER_STATE_AUTO, POPOVER_STATE_MANUAL, POPOVER_STATE_HINT };

/* §2.3.3's ATTRIBUTE DEFINITION for `popover` — the keyword table and the three special states together, which
   is what core/html/html_element.c's `popover` reflection row points at. */
extern const EnumeratedAttribute POPOVER_ATTRIBUTE;

/* §2.3.3's DETERMINE THE STATE for this element's `popover` attribute — one of the four above. */
int popover_attribute_state(const lxb_dom_element_t *el);

/* Declared ONCE PER AGENT (the slot keys and the three members' pool ids); installed onto each realm's
   HTMLElement.prototype, which core/html/html_element.c owns and hands over. */
void popover_declare(JSContext *ctx);
void popover_install(JSContext *ctx, JSValueConst html_proto);
void popover_free(JSRuntime *rt);

#endif
