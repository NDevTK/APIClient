/* Namespaces in XML 1.0 (Third Edition) §3, §5 and §6 — THE NAMESPACE SCOPE STACK.
 *
 * WHAT IT IS. The half of XML that no tree builder in this tree does. Lexbor's HTML tree construction resolves
 * namespaces from a FIXED table — an element is HTML, MathML or SVG because of where the insertion mode put it,
 * and `xmlns` is an ordinary attribute — because that is what HTML §13.2 says. XML says the opposite: a
 * document DECLARES its own bindings as it goes, they NEST, and the same prefix means different things at
 * different depths. §6.1: "The scope of a namespace declaration declaring a prefix extends from the beginning
 * of the start-tag in which it appears to the end of the corresponding end-tag, EXCLUDING THE SCOPE OF ANY
 * INNER DECLARATIONS WITH THE SAME NSAttName PART." That exclusion clause is the whole data structure: a stack
 * of frames, resolved innermost-first.
 *
 * IT OWNS THE CONSTRAINTS, NOT JUST THE LOOKUP, and that is why it is a component rather than a hash. Every
 * "Namespace constraint" in the standard is a rule about a binding or a use — §3's Reserved Prefixes and
 * Namespace Names, §5's Prefix Declared, §5's No Prefix Undeclaring — so each one is decided exactly where the
 * declaration is recorded or the name is expanded, and each has its own value in XmlNsError. There is no
 * "namespace error" bucket: an unbound prefix and a rebound `xml` are different sentences a page's author needs
 * to read, and collapsing them is the stub §NO STUBS forbids.
 *
 * A NAMESPACE CONSTRAINT IS NOT A WELL-FORMEDNESS CONSTRAINT, AND THE CONSEQUENCE DIFFERS — which is why none
 * of these is a DFAIL. XML 1.0 §1.2 defines "well-formedness constraint: A rule which applies to all
 * well-formed XML documents. Violations of well-formedness constraints are FATAL ERRORS", and a fatal error is
 * one after which a processor "MUST NOT continue normal processing (i.e., it MUST NOT continue to pass
 * character data and information about the document's logical structure to the application in the normal way)".
 * Namespaces in XML says no such thing. Its §1 calls an NSC "one of the rules that documents conforming to this
 * specification MUST follow", and §8 Conformance of Processors asks only that a processor "MUST REPORT
 * violations of namespace well-formedness". So this layer REPORTS and keeps going; what to DO with a report is
 * the consumer standard's, and HTML §8.5.1 is the consumer that discards the tree and builds a `parsererror`
 * document instead. That is also why these are returned values rather than aborts: a namespace-ill-formed
 * document is a page's INPUT, and §Offensive-programming's own carve-out is that a flow throwing on attacker
 * input is the exploration surface, never a broken engine invariant.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: §6.3's "Attributes Unique". That constraint is "no tag may contain two
 * attributes which have identical names, or have qualified names with the same local part and with prefixes
 * which have been bound to namespace names that are identical" — the second half is this component's expansion,
 * but the FIRST half is XML 1.0's own Unique Att Spec WFC and both halves need the tag's whole attribute LIST,
 * which the tree builder holds and this does not. It belongs with the attribute list, decided over the expanded
 * names this returns. Named here so the next component knows it is owed, not implemented here so it can be
 * decided over half the information.
 *
 * WHAT IT DOES NOT INTERN, AND WHY THE OBVIOUS BINDING IS WRONG. Lexbor ships `lxb_ns_prefix_append` and
 * `lxb_ns_data_by_link` over the document's own `prefix` and `ns` hashes, which is where a prefix and a
 * namespace name are interned for the DOM — and both are CASE-FOLDING. `lexbor_hash_insert_lower`'s copy is
 * `to[i] = lexbor_str_res_map_lowercase[key[i]]` and its compare is `lexbor_str_data_nlocmp_right`, and the
 * static-table probe both functions run first is `lexbor_shs_entry_get_lower_static`. That is right for HTML,
 * where a tag name is ASCII-lowercased by the tokenizer, and it contradicts this standard in both directions:
 *   - §2.3 states the namespace-name comparison and gives its own counter-example — "http://www.example.org/wine",
 *     "http://www.Example.org/wine" and "http://www.example.org/Wine" are "all different for the purposes of
 *     identifying namespaces, since they differ in case". Lexbor makes them one namespace.
 *   - §3 says `xmlns:xml="urn:x"` MUST NOT happen, while a prefix spelled `XML` is merely "reserved" and
 *     processors "MUST NOT treat them as fatal errors". `lxb_ns_prefix_append(hash, "XML", 3)` returns the
 *     static entry whose `prefix_id` is `LXB_NS_XML`, so the two become one prefix and a LEGAL document gets
 *     reported for a constraint it does not violate.
 * So this component does not intern at all. It stores BYTES and returns BORROWED SLICES, and the interning
 * happens where it already happens. THAT QUESTION IS NOW ANSWERED and this paragraph used to leave it open:
 * it said the insert is `lxb_dom_element_create`'s and that "whether THAT insert must stop case-folding for an
 * XML document is a real defect ... the tree builder's to answer". It is not an XML-only defect and it is no
 * longer open — core/dom/name_intern.h stores every one of these names AS GIVEN for the DOM as a whole, for the
 * three standards sentences above and for the DOM's own, and both `element_create_ns` and `dom_attr_create` go
 * through it. What this component still does not do is intern, for the reason it always gave.
 *
 * THE ARENA'S LIFETIME IS THE WHOLE PARSE, NOT THE FRAME'S, AND THAT IS THE POINT. Popping a scope does not
 * free the bytes its bindings copied. A slice this hands back is the namespace name of an element, and the
 * tree builder may hold it across the element's end-tag — so freeing on pop would dangle exactly the pointer
 * the consumer was given, at exactly the moment it stops looking. One `lexbor_mraw_t` per scope stack, freed
 * with it, makes "valid until xml_ns_scope_destroy" a lifetime with no cases in it. (The two reserved namespace
 * names below are string literals with static storage duration, so they satisfy that promise trivially.)
 *
 * THE STACK IS FLOW-PRIVATE STATE AND IS NEVER COW-CAPTURED — AND THAT IS NOT THE SAME CLAIM AS "NOTHING CAN
 * PARK INSIDE A PARSE", which is FALSE in this tree and worth getting right here rather than discovering later.
 *   The COW half is settled. §State-isolation: "The delta captures ONLY shared baseline state — flow-private
 * state is never captured", and "an object CREATED by the running flow can never be observed by another flow".
 * A scope stack is created by one parse, is named by nothing outside it, and is destroyed before that parse
 * returns; two flows parsing the same bytes each mint their own, so there is no shared slot for a delta to hold
 * two values of. Capturing it would put a parse's transients into a delta whose whole invariant is that it is
 * O(shared state touched). What DOES time-travel is the DOCUMENT the parse builds — solver/dom_cow.c captures
 * those tree writes — and that is the distinction to keep: the tree is shared and outlives the parse, the stack
 * is private and does not.
 *   THE FORK HALF IS OPEN, AND THE PRECEDENT IS ALREADY IN THIS TREE. HTML's fragment parse IS suspendable —
 * core/dom/element.c's FRAG_FEED stage runs "one byte per step" and core/html/tree_construction.c exists to
 * COPY an lxb_html_tree_t standing mid-parse — so "a parse is one uninterruptible C activation" is not a thing
 * this engine believes, and an XML tree builder that is a step machine (which §scheduler requires of it, since
 * a parse over attacker-length input is unbounded) will be forkable at every byte too. A forked arm needs its
 * OWN stack, and a byte copy is specifically the wrong answer: a binding's `prefix` and `ns` are interior
 * pointers into THIS stack's arena, so a copied binding names the original arm's bytes — the self-reference
 * class JSStepVisit::reexec exists to re-point. The owed work is therefore a visit declaration for this state
 * in whatever step machine holds it, and element.c's `frag_unforkable` is the precedent for what stands in
 * until then: the fork ABORTS naming what to build rather than two arms sharing one structure.
 *   SO THE CLAIM IS CHECKED, NOT ASSERTED IN PROSE. The stack records the flow that created it and every
 * operation DCHECKs that the running flow is still that one — one level below frag_unforkable and
 * unconditional, so it fires for any second flow however it got here, a fork that forgot to copy or a resume
 * that came back from a cold tier with the C memory gone. Building the parkable representation before there is
 * a step machine to park it in would be plumbing added to dodge plumbing; the assert is what makes its absence
 * impossible to run past. */
#ifndef APICLIENT_XML_NS_H
#define APICLIENT_XML_NS_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_name.h"

/* THE TWO NAMESPACE NAMES §3 FIXES BY DEFINITION. "The prefix xml is by definition bound to the namespace name
   http://www.w3.org/XML/1998/namespace"; "The prefix xmlns … is by definition bound to the namespace name
   http://www.w3.org/2000/xmlns/". They are the standard's own constants and are compared against by identity,
   which is why they are written out rather than reached through lexbor's table — a table lookup would make the
   comparison depend on whether the document happened to have interned them, and (see above) lexbor's lookup
   would answer for a differently-cased spelling too. DOM §1.4's steps 9 to 11 compare against the same two
   constants for the same reason, and read them from here so there is one definition of each. */
#define XML_NS_XML_NAMESPACE   "http://www.w3.org/XML/1998/namespace"
#define XML_NS_XMLNS_NAMESPACE "http://www.w3.org/2000/xmlns/"

/* WHICH CONSTRAINT WAS VIOLATED. One value per sentence of the standard, because that is what a report has to
   say — see the head comment on why this is a returned value and not an abort. Zero is OK so a caller may write
   `if (err)`, and every non-zero value has a message. */
typedef enum {
    XML_NS_OK = 0,
    /* §3 Namespace constraint: Reserved Prefixes and Namespace Names — five sentences, five values. */
    XML_NS_ERR_XML_PREFIX_REBOUND,               /* xmlns:xml="…" where … is not the XML namespace */
    XML_NS_ERR_XML_NAMESPACE_ON_OTHER_PREFIX,    /* xmlns:p="…XML/1998/namespace" for p other than xml */
    XML_NS_ERR_XML_NAMESPACE_AS_DEFAULT,         /* xmlns="…XML/1998/namespace" */
    XML_NS_ERR_XMLNS_PREFIX_DECLARED,            /* xmlns:xmlns="…" — unconditional, whatever the value */
    XML_NS_ERR_XMLNS_NAMESPACE_ON_OTHER_PREFIX,  /* xmlns:p="…2000/xmlns/" for p other than xmlns */
    XML_NS_ERR_XMLNS_NAMESPACE_AS_DEFAULT,       /* xmlns="…2000/xmlns/" */
    XML_NS_ERR_XMLNS_ELEMENT_PREFIX,             /* <xmlns:e/> — "Element names MUST NOT have the prefix xmlns" */
    /* §5 Namespace constraint: No Prefix Undeclaring */
    XML_NS_ERR_PREFIX_UNDECLARING,               /* xmlns:p="" — 1.0 has no undeclaring; 1.1 does */
    /* §5 Namespace constraint: Prefix Declared */
    XML_NS_ERR_PREFIX_UNDECLARED                 /* a prefix used with no declaration in scope */
} XmlNsError;

/* The constraint's own name and the sentence violated, for the well-formedness error record to report. Never
   NULL — XML_NS_OK has a message too, and a caller that formats it has asked the wrong question. */
const char *xml_ns_error_message(XmlNsError err);

/* §3 [1] `NSAttName ::= PrefixedAttName | DefaultAttName` — WHICH KIND OF DECLARATION AN ATTRIBUTE NAME IS.
   Asked of every attribute, because whether a name is a declaration is a fact about the NAME and the answer
   decides whether it binds or is bound. */
typedef enum {
    XML_NS_ATT_NONE = 0,   /* an ordinary attribute — [15]'s `QName Eq AttValue` arm */
    XML_NS_ATT_DEFAULT,    /* [3] DefaultAttName ::= 'xmlns' */
    XML_NS_ATT_PREFIXED    /* [2] PrefixedAttName ::= 'xmlns:' NCName — the NCName is the DECLARED prefix */
} XmlNsAttKind;

XmlNsAttKind xml_ns_att_kind(const XmlQName *attr_name);

/* WHICH KIND OF NAME IS BEING EXPANDED. §6.2: "Default namespace declarations do not apply directly to
   attribute names", and "The namespace name for an unprefixed attribute name ALWAYS has no value" — so an
   unprefixed element and an unprefixed attribute inside the same start-tag get different answers from the same
   scope. It is a named argument for core/dom/names.h's reason: passing the wrong one is a spec bug that no test
   of the other kind can see. */
typedef enum { XML_NS_NAME_ELEMENT, XML_NS_NAME_ATTRIBUTE } XmlNsNameKind;

/* §2.1's EXPANDED NAME — "a pair consisting of a namespace name and a local name". The prefix comes back too
   because the DOM keeps it (an Element remembers whether it was written `html:p` or `p`), not because the
   expanded name has one.
   `ns` is NULL for §6.2's "the namespace name has no value" — the standard's absence, never the empty string,
   which is a namespace name a document may not declare for a prefix at all. `ns` points into the scope stack's
   arena or at one of the two constants above, and is valid until xml_ns_scope_destroy; `prefix` and `local`
   are BORROWED from the caller's own qualified name. */
typedef struct {
    const char *ns;     size_t ns_len;
    const char *prefix; size_t prefix_len;
    const char *local;  size_t local_len;
} XmlExpandedName;

typedef struct XmlNsScope XmlNsScope;

/* One stack per parse. Records the running flow — see the head comment's last paragraph. */
XmlNsScope *xml_ns_scope_create(void);

/* THE TWO TEARDOWNS, core/xml/xml_element.h'S SPLIT AGAIN AND FOR THE SAME REASON. `destroy` is the stack
   every push of which was popped by its element's end-tag, which is the only shape a FINISHED parse leaves;
   it DCHECKs that. `abandon` is the stack a parse that stopped mid-element leaves — XML §1.2 Terminology's
   fatal error at any layer, or the flow driving the parse being gone — where open scopes are the expected
   residue and not a lost end-tag. Which one it is belongs to the caller: this stack is pushed and popped by
   the tree builder and has no way to tell a document that ended from one that was stopped. */
void        xml_ns_scope_destroy(XmlNsScope *s);
void        xml_ns_scope_abandon(XmlNsScope *s);

/* §6.1's scope, entered at the beginning of a start-tag and left at the end of the corresponding end-tag; "in
   the case of an empty tag, the scope is the tag itself", so an empty-element tag pushes and pops around its
   own attributes and name. Push BEFORE the tag's declarations are recorded and BEFORE its own QName is
   expanded — a declaration on a start-tag is in scope for that start-tag, which is what makes
   `<edi:price xmlns:edi='…'>` legal. */
void   xml_ns_push(XmlNsScope *s);
void   xml_ns_pop(XmlNsScope *s);
size_t xml_ns_depth(const XmlNsScope *s);

/* RECORD A NAMESPACE DECLARATION in the innermost scope, and decide §3's Reserved Prefixes and Namespace Names
   and §5's No Prefix Undeclaring over it.
   Takes the attribute's whole QNAME rather than the declared prefix, because for [2] PrefixedAttName the
   declared prefix is the LOCAL part (`xmlns:edi` declares `edi`, not `xmlns`) and making the caller extract it
   is the one place that mistake can be made. Asserts that the name IS a declaration — ask xml_ns_att_kind
   first; a non-declaration reaching here is a caller bug and not an input the standard has an answer for.
   The binding is recorded EVEN WHEN A CONSTRAINT IS REPORTED, so that resolution stays total and the errors a
   document accumulates are all of them rather than the first one plus a cascade of spurious
   XML_NS_ERR_PREFIX_UNDECLARED. `xmlns:p=""` is recorded as an UNBINDING of p (which is what Namespaces in XML
   1.1 makes legal and what 1.0 reports), so an inner empty declaration shadows an outer one either way. */
XmlNsError xml_ns_declare(XmlNsScope *s, const XmlQName *attr_name, const char *value, size_t value_len);

/* §6's EXPANSION of a qualified name against the scopes in force, deciding §5's Prefix Declared and §3's
   "Element names MUST NOT have the prefix xmlns" on the way. `out` is written only when XML_NS_OK is returned.

   NOTE WHAT THIS ANSWERS FOR `xmlns` AS AN UNPREFIXED ATTRIBUTE NAME, because two standards differ and this one
   is Namespaces in XML: §6.2's "the namespace name for an unprefixed attribute name always has no value" has no
   exception for `xmlns`, so that is what comes back. The DOM puts the declaration attribute in the XMLNS
   namespace instead — validate-and-extract step 10 makes the name `xmlns` REQUIRE it — and that is DOM §1.4's
   rule about a node it constructs, not this standard's rule about a document it reads. The tree builder applies
   it where it builds the Attr; it is not smuggled in here, where it would make `xml_ns_expand` disagree with
   the section it implements. */
XmlNsError xml_ns_expand(const XmlNsScope *s, const XmlQName *qname, XmlNsNameKind kind, XmlExpandedName *out);

#endif
