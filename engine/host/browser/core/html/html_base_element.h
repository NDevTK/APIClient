/* HTML §4.2.3 "The base element" — the FROZEN BASE URL, which is the half of `<base>` that was missing.
 *
 * `<base target>` was already answered, by a walk at each ask in core/html/html_form.c — and that shape is
 * CORRECT for `target` and WRONG for `href`, which is why this component exists rather than a second entry in
 * that walk. §4.6.3's "get an element's target" is stated as a lookup performed at the moment it is asked
 * ("otherwise, if element's node document contains a base element with a target attribute, set target to the
 * value of the target attribute of the FIRST such base element"), so a walk per ask IS the algorithm. §4.2.3's
 * href half is not: the URL is FROZEN — parsed once, against the document's FALLBACK BASE URL as it stood at
 * the freeze — and §4.2.3 lists exactly two situations that (re-)freeze it. A walk per ask would re-parse
 * against whatever the fallback base URL had become, so `history.pushState({}, "", "/other/")` after a
 * `<base href="x/">` would silently move every relative URL in the document. The frozen URL is therefore
 * STORED (core/dom/document.c holds it, beside the address it is not) and this file owns WHEN it is written.
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

#endif
