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

/* DOM §4.4 "Interface Node" — EVERY STRING A WALK ABOVE COMPARES ITS ARGUMENT AGAINST, LISTED IN THE ORDER
 * THE WALK ASKS. It is the walks' COMPARISON SITES enumerated and nothing else, which is why it is here and
 * not at the member: a list that drifts from the walk is a world the member never asks about, and the only
 * defence against that is that the two are read together.
 *
 * WHO NEEDS IT AND WHY THE WALK CANNOT ANSWER THEM. A member handed an argument that is unknown external
 * input carrying no example cannot run either walk, because every step of both walks decides by comparing
 * that argument with one of these strings — so the walk has no answer until the flow has one. The member asks
 * the flow instead, one string at a time, and the moment a link says YES the argument is a KNOWN string and
 * the ordinary walk answers. The list is therefore the QUESTION and the walk stays the ANSWER; nothing here
 * decides a namespace.
 *
 * THE CANDIDATES ARE THE ARGUMENT-SIDE OPERANDS OF THE `ns_eq`/`ns_eq_n` CALLS IN node_ns.c, AND THAT IS THE
 * WHOLE SPECIFICATION. `prefixes` true lists locate a namespace's four: the two names Namespaces in XML §3
 * binds by definition (its steps 1 and 2), each ancestor element's own namespace prefix (step 3) and each
 * `xmlns:` declaration's LOCAL NAME (step 4). `prefixes` false lists locate a namespace prefix's two: each
 * ancestor element's own namespace (its step 1) and each `xmlns:` declaration's VALUE (step 2). The
 * argument's OWN null — which locate a namespace reaches through the default-declaration shape of step 4 —
 * is not here, because the member is what maps the empty string onto it (§4.4's `lookupNamespaceURI` step 1
 * and `lookupPrefix` step 1) and node_ns.h's banner keeps that mapping at the member.
 *
 * IT IS A C LOCAL AND NEVER CROSSES A SUSPENSION. Its cursor is a POSITION in a tree the page mutates, which
 * is exactly what CLAUDE.md's §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED forbids a parked flow from
 * carrying — so a caller enumerates in ONE uninterrupted stretch, into storage of its own, and asks its
 * questions afterwards. Every string it yields is BORROWED from the tree on the same terms as the walks'
 * answers: valid for as long as the node it came from is, never NUL-terminated, and read through the length
 * this writes. */
typedef struct {
    lxb_dom_element_t *el;     /* the element whose sites are being listed, NULL once the chain is walked out */
    lxb_dom_attr_t    *attr;   /* the next attribute of `el` to consider */
    unsigned char      site;   /* which comparison site of the walk comes next */
    bool               prefixes;
} NodeNsCandidates;

/* Begin listing for `node` — the SAME node either member hands its walk, so this runs §4.4's own interface
   dispatch (node_ns_start_element) rather than making a caller pre-resolve an element and disagree with the
   walk about which one it is. */
void node_ns_candidates_begin(NodeNsCandidates *it, lxb_dom_node_t *node, bool prefixes);

/* The next candidate and its length, or NULL once every site has been listed — which is the walk falling
   through to its own "Return null", and is the answer a caller whose argument matched nothing must give. */
const char *node_ns_candidates_next(NodeNsCandidates *it, size_t *out_len);

#endif
