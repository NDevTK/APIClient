/* THE RULES FOR PARSING PSEUDO-ATTRIBUTES FROM A STRING — xml-stylesheet §3 Pseudo-attributes, whose own
 * definition sentence is "The rules for parsing pseudo-attributes from a string are given in this section."
 *
 * IT IS A GRAMMAR OVER A STRING AND NOTHING ELSE, which is why it is a component of its own rather than a
 * helper inside the node that needs it. Nothing here knows what a document, a node or a realm is: it takes
 * bytes and answers with pairs or with an error, so one fixture exercises it whole and every invariant it has
 * is about its own parse rather than about a caller's state.
 *
 * WHO ASKS. DOM §4.13 Interface ProcessingInstruction's "update attributes from data" is the consumer this was
 * written for — its step 2 is "Let result be the parsing result of invoking the rules for parsing
 * pseudo-attributes from a string given pi's data", and its step 3 is "If result is an error, then return",
 * which is the whole use a DOM caller makes of the distinction below. The finer error values exist for
 * core/xml/xml_ref.h's reason rather than for that caller: an unterminated value, a duplicate name and a
 * character reference to a code point that is no character are different mistakes an author has to be told
 * apart, and collapsing them at the producer means no consumer can ever tell them apart again.
 *
 * THE ANSWER IS ORDERED, AND THE STANDARD'S WORD FOR IT IS "SET". xml-stylesheet §3 says the parsing result is
 * "either a set of pseudo-attributes or an error", and a set has no order — but the only consumer walks it
 * with "For each pseudo-attribute pseudoAttr of result" and writes each into an ORDERED map, so the order the
 * walk sees is observable through DOM §4.13's getAttributeNames(). Document order is the only order this
 * parser can defensibly produce and the only one a browser could agree with, so the result is a LIST and says
 * so here. Reading "set" as a licence to reorder would make the member's answer depend on a hash.
 *
 * THE PRODUCTIONS, verbatim from xml-stylesheet §3:
 *   [1a] PseudoAtts       ::= PseudoAtt? ( S PseudoAtt )* S?
 *   [2]  PseudoAtt        ::= Name S? "=" S? PseudoAttValue
 *   [3]  PseudoAttValue   ::= ('"' ([^"<&] | CharRef | PredefEntityRef)* '"' |
 *                              "'" ([^'<&] | CharRef | PredefEntityRef)* "'")
 *   [4]  PredefEntityRef  ::= "&amp;" | "&lt;" | "&gt;" | "&quot;" | "&apos;"
 * `S`, `Name` and `CharRef` are not defined there and are not restated here: xml-stylesheet §2 says "The
 * productions in this specification use the same notation as used in the XML specification. Tokens in the
 * grammar that are not defined in this specification are defined in the XML specification." So `S` is
 * core/xml/xml_char.h's, `Name` is core/xml/xml_name.h's and `CharRef` is core/xml/xml_ref.h's — the same
 * spellings every other XML production in this engine is decided by, which is what stops a second copy of
 * [4] NameChar drifting away from the first.
 *
 * [4] PredefEntityRef IS NOT SPELLED HERE EITHER, and that is the same rule rather than an omission: its five
 * names are exactly XML §4.6 Predefined Entities' five, which core/xml/xml_ref.h already matches by exact
 * bytes and reports as XML_REF_PREDEFINED. A general entity reference is XML_REF_ENTITY and is NOT in [3]'s
 * alternatives at all, so it is a grammar error here — which is a real difference from an XML attribute value
 * and the reason this cannot simply call the attribute-value normalizer.
 *
 * WHAT IS DELIBERATELY NOT A DCHECK. Every malformed input is an ERROR RETURN and never an assert: the bytes
 * are a document's or a page's, so they are INPUT, and asserting on them would hand whoever wrote the
 * processing instruction an abort switch. The asserts here are about this component's own logic only. */
#ifndef APICLIENT_XML_PSEUDO_ATTR_H
#define APICLIENT_XML_PSEUDO_ATTR_H

#include <stddef.h>

/* WHICH SENTENCE OF xml-stylesheet §3 THE STRING VIOLATED. Zero is OK so a caller may write `if (err)`.
   The three non-grammar values are the section's own three extra sentences, each stated separately there:
   "If the given string is not matched by the PseudoAtts production, the parsing result is an error", "The
   parsing result is an error if the XML Legal Character well-formedness contraint is violated for any CharRef"
   (the misspelling is the standard's, quoted as written), and "The parsing result is an error if there are
   more than one pseudo-attribute with the same name". */
typedef enum {
    XML_PSEUDO_OK = 0,
    XML_PSEUDO_ERR_GRAMMAR,           /* [1a]/[2]/[3]: the string is not matched by PseudoAtts */
    XML_PSEUDO_ERR_LEGAL_CHARACTER,   /* [66] [WFC: Legal Character] violated by a CharRef */
    XML_PSEUDO_ERR_DUPLICATE_NAME,    /* two pseudo-attributes with the same name */
    XML_PSEUDO_ERR_CHARACTER          /* the character layer latched one — ask xml_char_error_message */
} XmlPseudoAttrError;

/* The sentence violated. Never NULL — XML_PSEUDO_OK has a message too, and a caller that formats it has asked
   the wrong question. XML_PSEUDO_ERR_CHARACTER's message says to ask the character reader, because those
   sentences are core/xml/xml_char.h's to word and a second copy is how two spellings drift apart. */
const char *xml_pseudo_attr_error_message(XmlPseudoAttrError err);

/* ONE PSEUDO-ATTRIBUTE. Both strings are OWNED and NUL-terminated, and both are BUILT rather than borrowed.
   The name could have been borrowed — core/xml/xml_ref.h borrows a Name for the reason it states, that §2.11
   rewrites only #xD and #xD is in neither [4] NameStartChar nor [4a] NameChar — but the VALUE cannot be, since
   xml-stylesheet §3 says "Each CharRef is replaced with the character it represents according to XML" and "The
   first and last character (the start and end quotes) are removed", so its characters are not the entity's
   bytes. One ownership rule for both fields is what makes the free below able to state one. */
typedef struct {
    char  *name;        /* OWNED */
    size_t name_len;
    char  *value;       /* OWNED */
    size_t value_len;
} XmlPseudoAttr;

/* THE PARSING RESULT'S NON-ERROR ARM, in document order. `items` is OWNED and may be NULL when `n` is 0. */
typedef struct {
    XmlPseudoAttr *items;
    size_t         n;
    size_t         cap;
} XmlPseudoAttrs;

/* PARSE `len` BYTES OF UTF-8 AS [1a] PseudoAtts.
   `s` must be a valid pointer even when `len` is zero — an empty string is a thing this grammar answers about
   (every part of [1a] is optional, so it matches and the result is the empty list) and not the absence of one.

   `*out` IS ALWAYS LEFT VALID AND IS ALWAYS THE CALLER'S TO FREE, on every answer including the errors. That
   is a deliberate departure from the "written ONLY when OK" contract core/xml/xml_char.h and
   core/xml/xml_ref.h state for their scalar out-parameters, and the reason is that this one OWNS HEAP: an
   error found at the last pseudo-attribute has already built the ones before it, so "untouched" would either
   leak them or hand back a struct the caller must not free while its predecessor did. Zeroed-and-empty on
   error is the one shape with a single free rule, and it is what DOM §4.13's step 3 wants anyway — that step
   returns having already run step 1, "Clear pi's attribute map". */
XmlPseudoAttrError xml_pseudo_attr_parse(const char *s, size_t len, XmlPseudoAttrs *out);

/* Release everything `_parse` built and leave `*a` zeroed, so a double free is a no-op rather than a crash and
   a caller may free on both arms of its own error branch. */
void xml_pseudo_attrs_free(XmlPseudoAttrs *a);

#endif
