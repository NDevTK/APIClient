/* THE XML SERIALIZATION ALGORITHM — DOM Parsing and Serialization §5.2.1 "XML Serialization", as ONE machine,
 * embeddable in whichever member IS it. See xml_serialize.c.
 *
 * WHICH LIST A BARE STEP NUMBER NAMES, stated once here rather than re-derived at each site. §5.2.1 holds
 * THREE sibling lists under one heading: the non-normative list of ways an XML serialization differs from an
 * HTML one, then `to produce an XML serialization of a Node node given a boolean require well-formed`
 * (5 steps), then `the XML serialization algorithm` (one step, whose 10 sub-items are the interface dispatch).
 * Every bare `§5.2.1 step N` below is the SECOND list, which is the only one of the three that has a step N
 * for N > 1; the dispatch is named as the dispatch and never by number.
 *
 * WHAT IT IS AND WHY IT IS NOT core/html/fragment_serializer.c. What separates it from HTML §13.3 Serializing
 * HTML fragments is stated by DOM Parsing §5.2.1 "XML Serialization" in its own words: "Elements and attributes
 * will always be serialized such that their namespace is preserved. In some cases this means that an existing
 * prefix, prefix declaration attribute or default namespace declaration attribute might be dropped, substituted
 * or changed. An HTML serialization does not attempt to preserve the namespace", and "Elements not in the HTML
 * namespace containing no children, are serialized using the empty-element tag syntax (i.e., according to the XML
 * EmptyElemTag production)". Neither question exists in §13.3 at all, and answering them is a per-element
 * NAMESPACE PREFIX MAP (§5.2.1.1.2 "The Namespace Prefix Map") copied down the tree plus a generated-prefix
 * counter shared across the whole serialization. So this is a second walk, not a flag on the first one.
 *
 * IT IS AN EMBEDDABLE ALGORITHM AND NOT A MEMBER, for the reason DOM §4.4's `clone a node` is one
 * (core/dom/node.h): §5.2.1 has TWO consumers in the standards and they are in different specifications.
 * HTML §8.5.8 The XMLSerializer interface's `serializeToString(root)` is "return the XML serialization of root
 * given false"; HTML §8.5.4 The innerHTML property's fragment serializing algorithm steps are "if context
 * document is an HTML document, return the result of HTML fragment serialization algorithm ...; return the XML
 * serialization of node given require well-formed", reached with require well-formed TRUE from the innerHTML
 * and outerHTML getters. A member that owned the walk privately would be a second transcription of §5.2.1 the
 * day the second consumer lands, and the two would disagree about prefixes.
 *
 * HOW TO EMBED IT: expand XML_SERIALIZE_ALGO_STAGES into the caller's own stage block with the caller's own
 * leading text and prefix, embed an XmlSerializeState, chain xml_serialize_visit_state into the caller's visit,
 * and hand xml_serialize_run the base of that block. Stage identity is the LABEL, so the list is expanded once
 * per caller with that caller's own words in front of §5.2.1's steps — which is what keeps two consumers'
 * stages from resolving to each other on a cross-session resume.
 *
 * THE WALK IS A HEAP LEVEL STACK AND EVERY LEVEL IS A REST POINT, because a tree is as deep and as wide as the
 * page says. §5.2.1.1 "XML serializing an Element node" step 19 recurses over children, and a C recursion
 * there is a stretch of the page's size with nowhere for the scheduler to park — CLAUDE.md §scheduler's
 * razor. One node per step, one attribute per step, and the level stack is what the recursion became.
 *
 * WHERE §5.2.1's `throw an exception` LANDS. §5.2.1 step 5 wraps the whole algorithm: "If an exception occurs
 * during the execution of the algorithm, then catch that exception and throw an 'InvalidStateError'
 * DOMException." Every inner throw in §5.2.1.1 through §5.2.1.8 is therefore observable ONLY as that one
 * exception, so this machine throws it at the site that detected the ill-formedness, with the offending
 * sentence as the message. A separate inner exception class would be a value no caller can ever see. */
#ifndef ENGINE_HOST_BROWSER_CORE_XML_XML_SERIALIZE_H
#define ENGINE_HOST_BROWSER_CORE_XML_XML_SERIALIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* §5.2.1.1.2 "The Namespace Prefix Map", AS ONE APPEND-ONLY ARRAY OF PAIRS, and the representation is the
 * reason "copy a namespace prefix map" costs nothing here.
 *
 * §5.2.1.1.2 defines the map as "an ordered map from strings or null to lists of strings", whose "keys are
 * namespaces, with null representing no namespace; the values are lists of prefixes that map to that
 * namespace", and every operation it defines is stated over the candidates LIST for one key: "Let candidates be
 * map[ns]", "Append prefix to candidates", "If candidates contains prefix, return true". A flat array of
 * (ns, prefix) pairs in INSERTION ORDER carries exactly that: the candidates list for a key is the subsequence
 * of pairs with that key, in the order they were added, and add's two arms — "Let candidates be « prefix »" for
 * a key the map does not contain, "Append prefix to candidates" for one it does — are the SAME append.
 *
 * AND THE LIST'S ORDER IS THE MRU RULE, not an artifact of the representation: "The last seen prefix for a
 * given namespace is at the end of its respective list", and retrieve returns "candidates[size of candidates
 * - 1]" when the preferred prefix is not among them. Insertion order answers both by one forward scan.
 *
 * AND §5.2.1.1 step 6's copy is then a LENGTH. The map is only ever mutated by "add a prefix", which appends,
 * and an element hands its own (post-addition) map to its children — so a child's map is its parent's map plus
 * a suffix, and the whole tree's maps live in one array with a saved length per level. A per-element deep copy
 * of a map of lists would be an allocation per element of a tree the page chose the size of.
 *
 * IT IS ALSO §5.2.1.1's LOCAL PREFIXES MAP, which is prefix→namespace rather than the other way round: the
 * pair is (key, value) in both, and which field is which is stated at each use. */
typedef struct { uint32_t key, val; } XmlSerPair;

/* A LEVEL OF THE WALK — an element whose children are being serialized, or the DocumentFragment/Document the
   algorithm was entered on. `node` is NULL for that root container, which is what makes an exhausted stack the
   end of the walk rather than an end tag with nothing to close. */
typedef struct {
    lxb_dom_node_t *node;    /* the element whose §5.2.1.1 step 20 end tag this level owes; NULL at the root */
    lxb_dom_node_t *limit;   /* the container to restore — what `node` was itself a child of */
    uint32_t        qname;   /* §5.2.1.1's `qualified name`, as step 20 writes it */
    uint32_t        ctx_ns;  /* §5.2.1's context namespace to restore, so a SIBLING is not given ours */
    int             map_n;   /* §5.2.1.1 step 6: the map length the copy was made at */
} XmlSerLevel;

/* THE WHOLE OF §5.2.1's STATE. Every pointer here is js_malloc'd and declared to xml_serialize_visit_state, so
   a fork gives each arm its own accumulator, its own pool, its own map and its own level stack — two arms that
   branch inside a page's `toString` have to serialize their own remaining nodes. */
typedef struct {
    /* THE WALK */
    lxb_dom_node_t *cur;      /* the node §5.2.1's dispatch is standing on */
    lxb_dom_node_t *limit;    /* the container `cur` is a child of; NULL when `cur` is the root itself */
    lxb_dom_attr_t *attr;     /* the attribute §5.2.1.1.1's and §5.2.1.1.3's per-attribute loops stand on */
    XmlSerLevel    *stack;
    int             sp, scap;

    /* THE OUTPUT — §5.2.1.1's `markup` and §5.2.1.1.3's `result`, accumulated once rather than per node. */
    char   *out;
    size_t  out_len, out_cap;

    /* THE STRING POOL. Every namespace, prefix and qualified name this algorithm compares is interned here,
       so "is equal to" is an id comparison and the map is a pair of ints. Id 0 is §5.2.1's `null` and is never
       a string: the empty string is interned like any other, which is what lets §5.2.1.1.1's "The empty string
       is a legitimate return value and is not converted to null" be a real distinction rather than a lost
       one. */
    char     *pool;
    size_t    pool_len, pool_cap;
    uint32_t *pool_ent;
    int       pool_n, pool_ecap;

    /* §5.2.1.1.2's map, §5.2.1.1's `local prefixes map`, and §5.2.1.1.3's `localname set`. */
    XmlSerPair *map;     int map_n, map_cap;
    XmlSerPair *local;   int local_n, local_cap;
    XmlSerPair *lnset;   int lnset_n, lnset_cap;

    /* THE FOUR ARGUMENTS §5.2.1 says every one of its algorithms is passed, plus the element in flight. */
    uint32_t ctx_ns;         /* the context namespace */
    int      prefix_index;   /* the generated namespace prefix index, shared by REFERENCE across the walk */
    bool     require_well_formed;

    uint32_t elem_ns;        /* §5.2.1.1 step 10's `ns` */
    uint32_t inherited_ns;   /* §5.2.1.1 step 9's `inherited ns` — the context namespace of this element's children */
    uint32_t qname;          /* §5.2.1.1 step 3's `qualified name` */
    uint32_t local_default;  /* §5.2.1.1 step 8's `local default namespace`; 0 is its null */
    bool     ignore_nsdef;   /* §5.2.1.1 step 5's `ignore namespace definition attribute` */
    bool     skip_end_tag;   /* §5.2.1.1 step 4's `skip end tag` */

    /* The interned constants this algorithm names by URI, so no comparison spells one out twice. */
    uint32_t ns_html, ns_xml, ns_xmlns, s_empty, s_xml, s_xmlns;

    int after;               /* the CALLER's stage this algorithm hands control back at */
} XmlSerializeState;

/* §5.2.1's STAGES, expanded into the caller's own block. `W` is the caller's leading text and `P` its prefix.
   THE LABEL IS THE STAGE'S IDENTITY (quickjs-step.h's JSTrampStepDef.steps), so the section numbers in these
   strings are what a parked flow resolves its rest point BY — which is why they are repaired to the edition
   this file implements rather than left reading as an older one. */
#define XML_SERIALIZE_ALGO_STAGES(X, P, W) \
    X(P##_DISPATCH, W " → DOM Parsing and Serialization §5.2.1 XML Serialization: the XML serialization " \
                      "algorithm's dispatch on node's interface, and §5.2.1.1 XML serializing an Element node " \
                      "steps 1-7 (the localName check and the per-element copy of the namespace prefix map)") \
    X(P##_RECORD,   W " → DOM Parsing and Serialization §5.2.1.1.1 Recording the namespace: one attribute " \
                      "of element's attribute list per step") \
    X(P##_NAME,     W " → DOM Parsing and Serialization §5.2.1.1 XML serializing an Element node steps 9-12 " \
                      "(the element's qualified name, and the one namespace declaration its start tag carries)") \
    X(P##_ATTRS,    W " → DOM Parsing and Serialization §5.2.1.1.3 Serializing an Element's attributes " \
                      "step 3: one attribute of element's attribute list per step") \
    X(P##_OPEN,     W " → DOM Parsing and Serialization §5.2.1.1 XML serializing an Element node steps 14-19 " \
                      "(the empty-element tag's solidus, the start tag's \">\", and the descent into a " \
                      "template element's template contents or into the node's children)") \
    X(P##_LEAF,     W " → DOM Parsing and Serialization §5.2.1.3 XML serializing a Comment node, §5.2.1.4 " \
                      "XML serializing a CDATASection node, §5.2.1.5 XML serializing a Text node, §5.2.1.7 XML " \
                      "serializing a DocumentType node and §5.2.1.8 XML serializing a ProcessingInstruction " \
                      "node (this node's own markup)") \
    X(P##_NEXT,     W " → DOM Parsing and Serialization §5.2.1.2 XML serializing a Document node, §5.2.1.6 " \
                      "XML serializing a DocumentFragment node and §5.2.1.1 XML serializing an Element node " \
                      "step 19 (the step to the next child in tree order)") \
    X(P##_CLOSE,    W " → DOM Parsing and Serialization §5.2.1.1 XML serializing an Element node step 20 " \
                      "(\"</\", the qualified name, \">\")")

/* WHICH OF §5.2.1's TWO SHAPES THE CALLER IS ASKING FOR, AND WHY IT IS AN ARGUMENT RATHER THAN A SECOND
 * ALGORITHM. §5.2.1's step 5 reaches the dispatch on the node's INTERFACE, and for a Document (§5.2.1.2 "XML
 * serializing a Document node") and a DocumentFragment (§5.2.1.6 "XML serializing a DocumentFragment node")
 * that dispatch is already "For each child of node's children: Append to markup the result of running the XML
 * serialization algorithm given child, inherited ns, map, prefix index, and require well-formed" — the
 * node's own markup is not part of the output because it has none. An
 * ELEMENT reached through the same entry serializes its own start and end tags around that concatenation
 * (§5.2.1.1), and BOTH answers have a consumer, so the shape is a question the caller answers rather than a
 * property of the node.
 *
 * WHERE THE SECOND ANSWER COMES FROM, AND WHERE THE STANDARD IS AGAINST ITSELF. HTML §8.5.4 The innerHTML
 * property's fragment serializing algorithm steps read, in full: "Let context document be node's node
 * document. If context document is an HTML document, return the result of HTML fragment serialization
 * algorithm with node, false, and « ». Return the XML serialization of node given require well-formed." The
 * HTML arm is HTML §13.3 Serializing HTML fragments, whose output is the CHILDREN of node and never node's own
 * tags; step 3 as written hands the same node to §5.2.1's interface dispatch, which for an Element includes
 * them. So one member's two arms disagree about whether the receiver's own tags are in the answer, and the
 * section itself is flagged in the standard's own words — "The innerHTML property has a number of outstanding
 * issues in the DOM Parsing and Serialization issue tracker, documenting various problems with its
 * specification."
 *
 * THE ORACLE DECIDES IT AND IT IS NOT AMBIGUOUS: web-platform-tests `domparsing/innerhtml-03.xhtml` asserts
 * that a `div` holding one `xmp` answers `<xmp xmlns="http://www.w3.org/1999/xhtml">...</xmp>` — the child's
 * markup, with the default namespace declaration on the CHILD, which is what a null context namespace at the
 * child's own entry produces and what a `div` wrapper would have absorbed. So the XML arm is children-only for
 * an Element too, which is also §5.2.1's own answer for the ShadowRoot the same getter is declared on. That is
 * the entry below, and it is stated as a caller's argument rather than buried as a special case so that the
 * consumer which genuinely wants the node itself — HTML §8.5.8 The XMLSerializer interface's
 * `serializeToString(root)`, "return the XML serialization of root given false" — keeps saying so. */
typedef enum {
    /* §5.2.1 step 5's dispatch on `node`'s own interface: an Element's answer carries its own tags. */
    XML_SERIALIZE_NODE = 0,
    /* §5.2.1.6 "XML serializing a DocumentFragment node"'s concatenation, applied to `node`'s children
       whatever `node`'s interface is — with §5.2.1.1 step 18's template substitution, so a `template`
       element answers with its template contents exactly as it does when the walk reaches it from above. */
    XML_SERIALIZE_CHILDREN,
} XmlSerializeEntry;

/* Begin `produce an XML serialization of a Node node given a flag require well-formed` — §5.2.1's steps 1-4 (the
   null context namespace, the new prefix map, "xml" added to it for the XML namespace, and the prefix index
   at 1).
   `base` is where the caller declared the stage block and `after` is the caller's own stage this resumes at,
   with the accumulated markup on the state. Every field the walk reads is placed before the first step that can
   allocate or throw. */
void xml_serialize_start(JSContext *ctx, JSStepHdr *hdr, XmlSerializeState *s, lxb_dom_node_t *node,
                         XmlSerializeEntry entry, bool require_well_formed, int base, int after);
/* ONE STAGE. JS_STEP_YIELD to rest, JS_STEP_ABRUPT having thrown §5.2.1 step 5's "InvalidStateError"; the
   finish sets `hdr->stage` to the caller's `after`, so a caller never tests for completion. */
int  xml_serialize_run(JSContext *ctx, JSStepHdr *hdr, XmlSerializeState *s, int base);
/* §5.2.1's result, as the DOMString the member returns. Valid only at the caller's `after` stage. */
JSValue xml_serialize_result(JSContext *ctx, const XmlSerializeState *s);
/* The caller's own visit chains into this — every allocation above is declared here, so the fork copies them
   and the teardown discharges them through this one list. */
void xml_serialize_visit_state(JSContext *ctx, XmlSerializeState *s, JSStepVisit *v);

#endif
