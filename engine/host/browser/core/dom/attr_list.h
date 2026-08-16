/* DOM §4.9's ATTRIBUTE LIST — the raw operations. See attr_list.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ATTR_LIST_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ATTR_LIST_H
#include <stdbool.h>
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

/* §4.9.2 "create an attribute" — a DETACHED Attr node (element null), which is what `createAttribute` and
   `createAttributeNS` return and what `setAttributeNode` is handed. No validation: §4.9.2 is a raw constructor
   and every caller that needs a name check has already run it. `ns`/`prefix` NULL is the null namespace, and a
   non-null prefix with a null namespace is unconstructible ("validate and extract" step 8) and DCHECKed. */
lxb_dom_attr_t *dom_attr_create(lxb_dom_document_t *doc, const char *ns, const char *prefix, const char *local);
/* §4.4 "clone a node" step 2's attribute half — a DETACHED copy of `src` belonging to `doc`, with the same
   namespace, prefix, local name, qualified name and value. Same document is an id copy; another document
   re-interns exactly those bytes. See attr_list.c for what lexbor's own attribute clone did to them. */
lxb_dom_attr_t *dom_attr_clone(lxb_dom_document_t *doc, const lxb_dom_attr_t *src);
/* §4.9 "change an attribute" step 2 — the VALUE, over an attribute that may or may not be on an element. */
void dom_attr_set_value(lxb_dom_attr_t *a, const char *val, size_t val_len);
/* §4.9 "append an attribute" steps 1 and 2, at a POSITION. `before` NULL appends at the end, which is what the
   algorithm itself always does — the position exists for the per-flow delta, whose job is to restore a list
   rather than to run an algorithm, and whose restore must put an attribute back at the index the page's
   `el.attributes[i]` read it from. */
void dom_attr_attach(lxb_dom_element_t *el, lxb_dom_attr_t *a, lxb_dom_attr_t *before);
/* §4.9 "remove an attribute" steps 2 and 3 — unlink and set its element to null. THE NODE SURVIVES: §9.4.7
   clears `element` and nothing else, and `removeAttributeNode`/`removeNamedItem` RETURN that attribute. */
void dom_attr_detach(lxb_dom_attr_t *a);
/* §4.9 "replace an attribute" steps 2 to 5 — IN PLACE, so the list position (and `NamedNodeMap` index order) is
   preserved, which is what makes it different from a detach plus an append. The two must already agree on
   (namespace, local name), which "set an attribute" step 3 guarantees by finding the old one at that key. */
void dom_attr_replace(lxb_dom_attr_t *old_a, lxb_dom_attr_t *new_a);
/* Free a DETACHED attribute nothing can reach any more — the per-flow delta's release for one the flow created,
   and the element teardown's release for one that was still in its list. It is the ONE point an Attr's death
   converges on (lexbor frees an Attr through a leaf destructor, not through the document's per-interface
   dispatcher), so it is where the two agent-wide maps keyed on the node's address are told: the wrapper identity
   map and the taint shadow. See attr_list.c. */
void dom_attr_destroy(lxb_dom_attr_t *a);

/* §4.9 "append an attribute" and "change an attribute" over one identity: create it when the element has no
   attribute with that (namespace, local name), otherwise write the new value into the one it has. `prefix` is
   NULL when there is none, and is only ever consulted for a newly created attribute — an existing attribute's
   prefix is its own and a value write does not rename it. Returns the attribute it wrote, and sets `*created`
   (may be NULL) when it had to make one: the delta owns what a flow creates, and the only place that fact is
   known without asking twice is here. */
lxb_dom_attr_t *dom_attr_write(lxb_dom_element_t *el, const char *ns, const char *prefix, const char *local,
                               const char *val, size_t val_len, bool *created);

/* THE PARSE BOUNDARY. HTML tree construction produces attributes in the NULL namespace; lexbor stamps them with
   the element's namespace instead, and the difference is only knowable at the boundary — afterwards a parsed
   attribute and a scripted `setAttributeNS(XHTML, …)` are indistinguishable. Run on the tree a parse just
   built, and on nothing else. */
void dom_attr_normalize_parsed(lxb_dom_node_t *root);

#endif
