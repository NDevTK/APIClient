/* DOM §4.9's ATTRIBUTE LIST — the raw operations. See attr_list.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ATTR_LIST_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ATTR_LIST_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* ─── §4.9 THE ATTRIBUTE LIST ─────────────────────────────────────────────────────────────────────────────
 *
 * AN ATTRIBUTE'S IDENTITY IS (NAMESPACE, LOCAL NAME). Every namespace-aware algorithm in §4.9 is keyed on that
 * pair — "get an attribute by namespace and local name", "set an attribute value", "remove an attribute by
 * namespace and local name" — while the QUALIFIED name is presentation: it is what `setAttribute` and
 * `getAttribute` match against, and what serialization prints. The two are not interchangeable, and treating
 * the qualified name AS the identity is what makes a namespace impossible to round-trip: restoring
 * `xlink:href` by its qualified name creates a SECOND attribute, in the null namespace, wearing the same name.
 *
 * These are the RAW tree operations, deliberately without per-flow capture. dom_cow.c's chokepoint captures the
 * baseline and then writes THROUGH here, and its revert/unapply/apply restore through here too — so the delta
 * can put an attribute back in the namespace it was in. A caller that wants the capture calls the chokepoint;
 * there is no second way into the tree, because the identity is resolved in one place.
 *
 * THE NULL NAMESPACE IS `LXB_NS__UNDEF`, WHICH IS WHAT THE HTML PARSER ALREADY PRODUCES and what lexbor's
 * serializer already reads. `lxb_dom_element_set_attribute` is the outlier — it copies the ELEMENT's namespace
 * onto the attribute it creates, so a scripted `el.setAttribute("href", …)` on an HTML element produced an
 * attribute in the XHTML namespace that `getAttributeNS(null, "href")` cannot find and whose `namespaceURI`
 * must still answer null. Nothing in this engine calls it any more. */

/* An attribute's namespace URI, NULL for the null namespace. Not NUL-terminated: `len` is the length. */
const lxb_char_t *dom_attr_ns(const lxb_dom_attr_t *a, size_t *len);
/* An attribute's namespace prefix, NULL when it has none. Not NUL-terminated. */
const lxb_char_t *dom_attr_prefix(const lxb_dom_attr_t *a, size_t *len);

/* §4.9 "get an attribute by namespace and local name". `ns` NULL selects the null namespace. */
lxb_dom_attr_t *dom_attr_get_ns(lxb_dom_element_t *el, const char *ns, const char *local);
/* §4.9 "get an attribute by name" — the FIRST attribute whose QUALIFIED name is `qname`. */
lxb_dom_attr_t *dom_attr_get_qname(lxb_dom_element_t *el, const char *qname);

/* §4.9 "append an attribute" and "change an attribute" over one identity: create it when the element has no
   attribute with that (namespace, local name), otherwise write the new value into the one it has. `prefix` is
   NULL when there is none, and is only ever consulted for a newly created attribute — an existing attribute's
   prefix is its own and a value write does not rename it. */
void dom_attr_write(lxb_dom_element_t *el, const char *ns, const char *prefix, const char *local,
                    const char *val, size_t val_len);
/* §4.9 "remove an attribute by namespace and local name". A no-op when there is no such attribute. Takes the
   context because an Attr is a WRAPPED node: the removal frees it, and the identity map must be told before it
   is left naming freed memory. */
void dom_attr_erase(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* THE PARSE BOUNDARY. HTML tree construction produces attributes in the NULL namespace; lexbor stamps them with
   the element's namespace instead, and the difference is only knowable at the boundary — afterwards a parsed
   attribute and a scripted `setAttributeNS(XHTML, …)` are indistinguishable. Run on the tree a parse just
   built, and on nothing else. */
void dom_attr_normalize_parsed(lxb_dom_node_t *root);

#endif
