/* DECLARATIVE SHADOW ROOTS — HTML §4.12.3's `<template>` shadow-root content attributes and §13.2.6.4.4's
   template start-tag steps in the "in head" insertion mode. See declarative_shadow.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DECLARATIVE_SHADOW_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DECLARATIVE_SHADOW_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"

void declarative_shadow_init(JSContext *ctx);
void declarative_shadow_free(void);   /* the agent's: this component holds no per-realm value of its own */
/* §4.12.3's two ENUMERATED reflections, on HTMLTemplateElement and nowhere else. The three BOOLEAN ones are
   plain `[Reflect] boolean` and are rows of html_element.c's own table, where every other reflection is. */
void declarative_shadow_install_template_members(JSContext *ctx, JSValueConst template_proto);

/* HTML §13.2.6.4.4 "A start tag whose tag name is template", over the tree a parse just built.
 *
 * `tree` is the node the parse produced — a Document, or a fragment parse's root element. `topmost` is
 * §13.2.4.2's "topmost element in the stack of open elements", which the step's third condition names and which
 * a finished parse no longer has: for a document it is the document element, for a fragment parse it is the
 * root element the fragment parsing algorithm created. `allow` is the parser's "allow declarative shadow
 * roots", which is the DOCUMENT's — true for a navigation, false for `createHTMLDocument`, `DOMParser`, XHR's
 * `responseXML` and every fragment parse that did not opt in.
 *
 * WHY IT IS AT THE PARSE BOUNDARY AND NOT IN TREE CONSTRUCTION. The step's effect — "attach a shadow root" —
 * writes the (element -> shadow root) association on the element's WRAPPER, because §3.7 makes that a per-flow
 * fact (shadow_root.c states why). A wrapper's prototype is its document's REALM's, and this engine installs a
 * document's realm AFTER its lexbor parse returns: the parse cannot mint one. So the step runs at the same seam
 * `dom_attr_normalize_parsed` runs at — on the tree the parse just built, before anything can read it — which
 * is the seam this engine already treats as the end of tree construction. Nothing observes the difference: the
 * parse runs no page code (element.c's fragment machine states the same invariant), so there is no moment
 * between the two at which a page could see the `<template>` the standard never inserts. */
void declarative_shadow_parsed(JSContext *ctx, lxb_dom_node_t *tree, const lxb_dom_node_t *topmost, bool allow);

#endif
