/* HTML §4.2.6 — THE `style` ELEMENT: its associated CSS style sheet, and the two members that read one.
 *
 * WHAT THIS OWNS is the ASSOCIATION — "update a style block", the algorithm that decides whether a `<style>`
 * element has a CSS style sheet at all and rebuilds it whenever the answer could have changed. The sheet OBJECT
 * is core/css/css_style_sheet.h's; this is the standard that creates one, and it is a different standard from
 * the one that defines it because a `<link>`, an `@import` and `new CSSStyleSheet()` each create one their own
 * way. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_STYLE_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_STYLE_ELEMENT_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declares the members and REGISTERS §4.2.6's children-changed steps — the trigger that makes a rewritten
   `<style>` body produce a new style sheet. */
void html_style_element_init(JSContext *ctx);
/* `sheet` (CSSOM §6.3.2's LinkStyle mixin, which HTMLStyleElement includes) and `disabled` (§4.2.6's own, which
   is a forwarding to the sheet's disabled flag and NOT a content-attribute reflection). */
void html_style_element_install(JSContext *ctx, JSValueConst proto);
void html_style_element_free(JSContext *ctx);

/* §4.2.6's "UPDATE A STYLE BLOCK" for `el`, run whenever one of the three conditions the standard lists occurs.
   Not every element is a `<style>`: the test is this function's, so a caller at a tree seam names the seam and
   not the tag.
   IT TAKES NO CONTEXT ON PURPOSE. §4.2.3's steps belong to the node's DOCUMENT and not to whoever performed the
   write — two same-origin documents are one agent, so the flow appending a `<style>` is routinely not in the
   realm the sheet must be built in — and one of the two seams that reach this (§4.2.3's children changed steps)
   has only the mutating realm to offer. Resolving it from the element is the one answer that is right at both. */
void html_style_element_update(lxb_dom_element_t *el);

/* §4.2.6's FIRST TRIGGER, for a tree the PARSER built. "The element is popped off the stack of open elements of
   an HTML parser or XML parser" is the condition, and a Lexbor parse has no per-token seam — so the markup's own
   `<style>` elements get their style sheets here, before the document's first script can read one. The other two
   triggers need no entry point: one is §4.2.3's insertion/removing steps (the tree-steps drain calls
   html_style_element_update directly) and one is §4.2.3's children changed steps, which this component
   REGISTERS for in its init. */
void html_style_element_parsed(JSContext *ctx, lxb_dom_node_t *root);

#endif
