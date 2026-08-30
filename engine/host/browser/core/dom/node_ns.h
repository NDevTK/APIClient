/* DOM §4.4 "Interface Node" — LOCATE A NAMESPACE and LOCATE A NAMESPACE PREFIX, the two walks the three
 * namespace members are defined over. See core/dom/node_ns.c.
 *
 * WHY IT IS A COMPONENT AND NOT THE BODY OF `lookupNamespaceURI`. §4.4 does not define three members; it
 * defines TWO ALGORITHMS and then three members that are each a coercion plus one call into them —
 * "lookupNamespaceURI(prefix): if prefix is the empty string, then set it to null; return the result of
 * running locate a namespace for this using prefix", and isDefaultNamespace is that same call with a null
 * prefix and a comparison on the way out. Written as one member body with a magic, the SHARED half is the
 * algorithm and the magic selects the coercion — which is backwards, and is how the walk ends up existing once
 * per member that happens to need it.
 *
 * IT IS ALSO NOT ONLY THE MEMBERS' ALGORITHM, WHICH IS THE OTHER HALF OF WHY IT IS A FILE. HTML §14.4 "Parsing
 * XML fragments" step 4 feeds an XML parser "the string corresponding to the start tag of context, declaring
 * all the namespace prefixes that are in scope on that element in the DOM, as well as declaring the default
 * namespace (if any) that is in scope on that element" — and it does not leave "in scope" to the reader. It
 * DEFINES it, twice, by naming this file's two consumers: "A namespace prefix is in scope if the DOM
 * lookupNamespaceURI() method on the element would return a non-null value for that prefix", and "the default
 * namespace is the namespace for which the DOM isDefaultNamespace() method on the element would return true".
 * So the set §14.4 declares on its synthetic start tag is a fact about THIS walk, and a second walk written
 * beside it would be a second answer to a question the standard states has one.
 *
 * WHAT IT IS NOT: core/xml/xml_ns.h. That component is the PARSER's scope stack — Namespaces in XML §6.1's
 * "the scope of a namespace declaration … extends from the beginning of the start-tag in which it appears to
 * the end of the corresponding end-tag", which is a fact about BYTES being read and lives exactly as long as
 * the parse. This is the same question asked of a TREE that already exists, where the nesting is `parent`
 * links rather than a stack and the declarations are Attr nodes rather than tokens. They agree on the two
 * namespace names §3 fixes by definition, which is why those constants are read from there and not restated.
 *
 * THE ANSWER IS A BORROWED SLICE AND IS NOT NUL-TERMINATED. It points at an element's interned namespace name
 * or at an attribute's value — both owned by the document — or at one of xml_ns.h's two static constants, so
 * it is valid for as long as the node it came from is, and a caller that mutates the tree between the call and
 * the read has read the wrong thing. That is why every entry takes a length out-parameter and DCHECKs it. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_NS_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_NS_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* DOM §4.4's INTERFACE DISPATCH, which the standard writes twice — once inside "locate a namespace" and once
   inside the `lookupPrefix(namespace)` member steps — with the same five arms and the same five answers. An
   Element asks itself, a Document asks its document element, a DocumentType or DocumentFragment asks nothing,
   an Attr asks its element, and anything else asks its PARENT ELEMENT (the parent if it is an element, never
   the nearest ancestor that is one). NULL where the standard returns null. It is exported because
   `lookupPrefix` needs the element to hand to the algorithm below, and a second copy of this switch beside it
   is two spellings of one rule. */
lxb_dom_element_t *node_ns_start_element(lxb_dom_node_t *node);

/* DOM §4.4 "Interface Node" — "to locate a namespace for a node using prefix". `prefix` is the standard's
   argument: NULL is its null (which asks for the DEFAULT namespace), and the EMPTY STRING is a different
   argument the members map onto null before calling — that mapping is §4.4's step 1 and belongs to the member,
   not here, so this entry is given whichever the member decided. Returns the namespace name and writes its
   length, or NULL for the standard's null. */
const char *node_locate_namespace(lxb_dom_node_t *node, const char *prefix, size_t prefix_len, size_t *out_len);

/* DOM §4.4 — "to locate a namespace prefix for an element using namespace". Takes an ELEMENT because the
   algorithm does; the member is what routes a Document, an Attr or a Text node to one. Returns the prefix and
   writes its length, or NULL for the standard's null. */
const char *node_locate_namespace_prefix(lxb_dom_element_t *element, const char *ns, size_t ns_len,
                                         size_t *out_len);

#endif
