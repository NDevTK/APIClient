/* HTML §4.2.3 "The base element" — the FROZEN BASE URL, and §4.2.3's own "get an element's target".
 *
 * THE TWO HALVES OF `<base>` ARE ASKED DIFFERENTLY, AND THAT IS THE WHOLE SHAPE OF THIS FILE. §4.2.3's "get an
 * element's target" is stated as a lookup performed at the moment it is asked ("otherwise, if element's node
 * document contains a base element with a target attribute, set target to the value of the target attribute of
 * the FIRST such base element"), so a walk per ask IS the algorithm — and it lives at the bottom of this file
 * for its two callers. §4.2.3's href half is not: the URL is FROZEN — parsed once, against the document's
 * FALLBACK BASE URL as it stood at the freeze — and §4.2.3 lists exactly two situations that (re-)freeze it. A
 * walk per ask would re-parse against whatever the fallback base URL had become, so
 * `history.pushState({}, "", "/other/")` after a `<base href="x/">` would silently move every relative URL in
 * the document. The frozen URL is therefore STORED (core/dom/document.c holds it, beside the address it is not)
 * and this file owns WHEN it is written.
 *
 * THREE TRIGGERS, WHICH ARE THE TWO SITUATIONS §4.2.3 NAMES PLUS THE PARSE. "The base element becomes the
 * first base element in tree order with an href content attribute in its Document" is reached by an
 * INSERTION, by a REMOVAL (the one in front of it leaves and a later one becomes first), and by an ATTRIBUTE
 * change (an `href` appears on an element that precedes the current first, or leaves the current first);
 * "…and its href content attribute is changed" is the second situation and is the same attribute seam. The
 * third is not a situation at all — a Lexbor parse has no per-token hook, so the tree the parser built gets
 * its freeze in one walk, exactly where core/html/media_element.c's and core/html/autofocus.c's parsed walks
 * stand and for the identical reason.
 *
 * THE ELEMENT'S OWN `href` GETTER IS A THIRD ALGORITHM AND NOT EITHER OF THE OTHER TWO — §4.2.3 states it out:
 * it parses the content attribute against the document's FALLBACK base URL, "thus the base element isn't
 * affected by other base elements or itself". So it is neither §2.6.1's URL reflection (which resolves against
 * the document BASE url, and would make `<base href="a/"><base href="b/">`'s second element report the first
 * one's answer) nor a plain string reflection (which was what stood here, and reported the raw attribute). */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_BASE_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_BASE_ELEMENT_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/dom/node.h"

/* The AGENT's half: §4.2.3's `href` setter, declared once per runtime beside every other IDL member's. */
void html_base_element_init(JSContext *ctx);
/* §4.2.3's `[CEReactions, ReflectSetter] attribute USVString href` on HTMLBaseElement.prototype. Handed the
   prototype by core/html/html_element.c, which owns the table of which interface a tag wears, for the reason
   §4.12.1's `async` is: the member is not a reflection and this file owns the algorithm behind it. */
void html_base_element_install(JSContext *ctx, JSValueConst proto);
/* The agent-lifetime release — see the definition for why it takes no runtime. */
void html_base_element_free(void);

/* §4.2.3's FIRST SITUATION, reached through DOM §4.2.3's insertion and removing steps — registered on
   core/dom/node.h's tree-steps list, so a tree write cannot reach the tree without it. */
void html_base_element_tree_steps(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase);
/* §4.2.3's SECOND SITUATION (and the first, when an `href` appears on or leaves an element that is not the
   frozen one), reached through DOM §4.9's attribute change steps — registered on core/dom/element.c's
   element_attr_changed beside media_element_attr_changed. */
void html_base_element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* THE SAME FREEZE FOR THE TREE THE PARSER BUILT. A browser sets the frozen base URL while tree construction
   inserts the element, so a `<base href>` in the page's own markup is in force before the first script runs
   AND before anything else in the document resolves a URL — which is why core/dom/document.c runs this FIRST
   among its parsed walks: §4.8.5's iframe walk resolves `src`, §4.8.11.2's media walk resolves `src`, and
   §4.12.1's scripts resolve theirs, all against the document base URL this decides. `root` is the DOCUMENT
   node, because §4.2.3 says "in its Document" and a `<base>` the parser put outside `<head>` is still in it. */
void html_base_element_parsed(JSContext *ctx, lxb_dom_node_t *root);

/* HTML §4.2.3 "The base element" — GET AN ELEMENT'S TARGET, given an `a`, `area` or `form` `element` and an
 * optional explicit `target` (HTML §4.10.22.3 "Form submission algorithm" step 17's formTarget — the
 * submitter's `formtarget` when it is a submit button carrying one; NULL, with `explicit_len` ignored, when
 * there is none, which is what every other caller passes). Answers the target string and writes its length to
 * `plen`; NEVER NULL — the algorithm's own null return is the empty string here, which is what HTML §4.6.5
 * "Following hyperlinks" step 2 initialises targetAttributeValue to and what HTML §7.3.1.7 "Navigable target
 * names" step 4 already answers with the CURRENT navigable.
 *
 * IT LIVES HERE BECAUSE §4.2.3 IS WHERE IT IS DEFINED, and step 1's `<base target>` lookup is why it cannot
 * live anywhere else: the walk per ask IS the algorithm for this member, so a component owning only step 2's
 * reset would leave the walk in one caller and there would be two answers to "what target does this element
 * have". There were two, and they disagreed about BOTH steps — an `a` element read its own attribute and never
 * looked for a `<base>` at all, a `form` walked for one, and neither ran step 2.
 *
 * STEP 2 IS THE SECURITY HALF, AND IT IS A FACT ABOUT THE ELEMENT PATH AND NOT ABOUT THE NAME. A target that
 * contains BOTH an ASCII tab or newline and a U+003C (<) is reset to "_blank" — that shape is the tail of an
 * unterminated attribute in an HTML injection, so honouring it names a navigable after smuggled markup, and a
 * later `window.open("", "<name>")` from the injected page then addresses it. `window.open`'s own target is NOT
 * reset: §7.2.2.1's window open steps never run this algorithm, and `dangling-markup-window-name.html` asserts
 * that difference in four subtests. A caller that took the reset for a property of the NAME would break
 * `window.open` in the same edit that fixed `a`.
 *
 * THE RETURNED POINTER IS BORROWED — from the element's or the `<base>`'s Lexbor attribute buffer, from
 * `target`, or from a static literal — so it is valid until the next DOM write, and a caller that outlives one
 * copies it. */
const char *html_base_element_get_target(lxb_dom_node_t *element, const char *target, size_t explicit_len,
                                         size_t *plen);

/* §4.2.3 step 2's SHAPE, exported so that the one definition of it is also what the CONSUMER asserts against:
   core/frame/navigable.c's rules for choosing a navigable crash on an element-supplied name that still carries
   it, because such a name never came through the algorithm above. "ASCII tab or newline" is Infra §4.6 "Code
   points"' — U+0009 TAB, U+000A LF, or U+000D CR. */
bool html_base_element_target_is_dangling(const char *target, size_t len);

#endif
