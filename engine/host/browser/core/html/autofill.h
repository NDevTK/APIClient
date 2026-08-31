/* HTML §4.10.19.7 "Autofill" — §4.10.19.7.2 Processing model's IDL-EXPOSED AUTOFILL VALUE, which is the whole
 * of what `input.autocomplete`, `select.autocomplete` and `textarea.autocomplete` answer with. See autofill.c.
 *
 * ONE PROBLEM: §4.10.19.7.2 Processing model ends "The autocomplete IDL attribute, on getting, must return the
 * element's IDL-exposed autofill value" — a value the section DERIVES from the attribute through a 35-step
 * algorithm over a 57-row token table, not the attribute's bytes. All three members were `REFLECT_STRING` rows,
 * which is a wrong VALUE and not a lenient reading: the algorithm's DEFAULT branch sets the IDL-exposed
 * autofill value to THE EMPTY STRING, so `<input autocomplete="banana">` answers "" in every browser and
 * answered "banana" here, and `<input autocomplete="SHIPPING TEL">` answers "SHIPPING TEL" (the algorithm
 * composes the page's own tokens and lowercases only a `section-` prefix) while `<input autocomplete="tel
 * shipping">` — the same two tokens in the order the grammar does not accept — answers "".
 *
 * WHY IT IS NOT A `[Reflect]` ROW AND NOT AN ENUMERATED ONE EITHER. §2.6.2 declares all three `[CEReactions,
 * ReflectSetter] DOMString`: the SETTER reflects and the getter is this algorithm, which is core/dom/element.h's
 * stated reason such a member is a component. It is also not §2.6.1's limited-to-only-known-values kind, which
 * answers a CANONICAL KEYWORD from a keyword table — the autofill value is a SEQUENCE of tokens the page wrote,
 * and no keyword table has a state for `section-billing shipping tel`.
 *
 * IT IS NOT §4.10.3's `form.autocomplete`, WHICH IS AN ENUMERATED ATTRIBUTE AND STAYS A ROW. One name, two
 * attributes: the `form` element's is on/off with the On state as both defaults (core/html/html_form.h says so
 * at HTML_FORM_AUTOCOMPLETE_ATTRIBUTE), and a control's is §4.10.19.7's autofill detail tokens. The `form`
 * one's only appearance in THIS algorithm is a branch that decides the autofill FIELD NAME, which is not the
 * value this file answers — see autofill.c.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_AUTOFILL_H
#define ENGINE_HOST_BROWSER_CORE_HTML_AUTOFILL_H

#include "quickjs.h"

/* Once per AGENT — the `autocomplete` `[ReflectSetter]` setter's id, shared by all three interfaces. */
void autofill_init(JSContext *ctx);
/* Onto ONE realm's HTMLInputElement, HTMLSelectElement or HTMLTextAreaElement prototype. All three declare the
   same member over the same algorithm, so one install serves each. */
void autofill_install(JSContext *ctx, JSValueConst proto);
void autofill_free(void);

#endif
