/* HTML §4.2.5.1 "Standard metadata names"' `referrer`: the referrer policy a `<meta name=referrer>` delivers.
 * See html_meta_referrer.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_META_REFERRER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_META_REFERRER_H

#include <lexbor/dom/dom.h>

/* The algorithm HTML §4.2.5.1 runs when a meta element "is inserted into the document, or has its name or
   content attributes changed", up to its last step: the Referrer Policy §3 token this node sets, or NULL where
   a step returns. The last step, "set element's node document's policy container's referrer policy", is the
   caller's (core/dom/document.c), because which container that is follows from the node's document. BORROWED:
   the answer is a literal. */
const char *html_meta_referrer_policy(const lxb_dom_node_t *node);

#endif
