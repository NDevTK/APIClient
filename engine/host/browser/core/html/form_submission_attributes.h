/* HTML §4.10.19.6 "Form submission attributes" — the TWO members of that section whose getter is its own
 * algorithm. See form_submission_attributes.c.
 *
 * ONE PROBLEM: §4.10.19.6 states a pair of getter steps, twice, in the same words, for two attributes on three
 * interfaces — `action` on §4.10.3's `form`, and `formAction` on §4.10.5's `input` and §4.10.6's `button`:
 *
 *   "Let attribute be this's action attribute. If attribute is null or attribute's value is the empty string,
 *    then return this's node document's URL. Let urlString be the result of encoding-parsing-and-serializing a
 *    URL given attribute's value, relative to this's node document. If urlString is not failure, then return
 *    urlString. Return attribute's value, converted to a scalar value string."
 *
 * ALL THREE WERE `REFLECT_STRING` ROWS, and that is a wrong VALUE twice over rather than a lenient reading. The
 * mirror answered the RAW attribute, so `<form action="/x">.action` read `/x` where every browser reads
 * `https://host/x` — the member through which a page reads back where its own form posts, and through which
 * this engine learns that endpoint. And `<form>.action` read the empty string where a browser reads the
 * document's own address, so a form that submits to the current page reported no destination at all. Both are
 * invisible to a member-presence audit: the names were installed and only the declared kind said what value
 * they would answer with.
 *
 * WHY IT IS NOT A NEW REFLECTION KIND. §2.6.2 declares all three `[CEReactions, ReflectSetter] USVString`, and
 * core/dom/element.h's reflection enum states the rule this obeys: `[ReflectSetter]` means the SETTER reflects
 * and the getter is the member's own algorithm, so a kind carrying both directions would have nothing to answer
 * the getter with. What the section changes is exactly §2.6.1's step 1 — an absent-or-empty attribute answers
 * the DOCUMENT'S URL where the reflection answers "" — and steps 2 and 3 are word for word §2.6.1's, which is
 * why they are called rather than copied (core/dom/element.h's element_reflect_url_get).
 *
 * WHY THESE TWO AND NOT THE SECTION'S OTHERS. §4.10.19.6 also states `formmethod`, `formenctype`, `formtarget`
 * and `formnovalidate`. Every one of those answers from the attribute alone — the first two are §2.6.1's
 * limited-to-only-known-values reflection over §2.3.3 keyword tables, the third a plain `DOMString` mirror, the
 * last a boolean — so they are registry rows in core/html/html_element.c and belong there. The test is
 * core/dom/element.h's: a kind must answer BOTH directions from the attribute alone, and only these two cannot.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FORM_SUBMISSION_ATTRIBUTES_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FORM_SUBMISSION_ATTRIBUTES_H

#include "quickjs.h"

/* Once per AGENT — the two members' `[ReflectSetter]` setter ids. */
void form_submission_attributes_init(JSContext *ctx);
/* Install ONE of the two onto the interface prototype that DECLARES it — "action" on HTMLFormElement,
   "formAction" on HTMLInputElement and HTMLButtonElement. The member NAME is what a caller names, because the
   content attribute it views and the receiver it brands against are this component's to know; a name
   §4.10.19.6 does not state is a DFAIL rather than a member over an attribute nothing writes. */
void form_submission_attributes_install(JSContext *ctx, JSValueConst proto, const char *member);
void form_submission_attributes_free(void);

#endif
