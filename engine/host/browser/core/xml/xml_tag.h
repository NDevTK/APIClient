/* XML 1.0 (Fifth Edition) §3.1 Start-Tags, End-Tags, and Empty-Element Tags, with §3.3.3 Attribute-Value
 * Normalization over §2.3's [10] `AttValue` — THE FIRST GRAMMAR RULE OF THIS COMPONENT SET RATHER THAN ANOTHER
 * LEAF.
 *
 * WHAT IT IS. Three productions and the rule that turns the third into text: [40] `STag ::= '<' Name (S
 * Attribute)* S? '>'`, [42] `ETag ::= '</' Name S? '>'`, [44] `EmptyElemTag ::= '<' Name (S Attribute)* S?
 * '/>'`, each built out of [41] `Attribute ::= Name Eq AttValue`, §2.3's [25] `Eq ::= S? '=' S?` and its [10]
 * `AttValue ::= '"' ([^<&"] | Reference)* '"' | "'" ([^<&'] | Reference)* "'"`. It sits on
 * core/xml/xml_char.h for [2] Char, [3] S and §2.11, on core/xml/xml_name.h for [5] Name, and on
 * core/xml/xml_ref.h for [67] Reference — and on NOTHING ELSE.
 *
 * [40] AND [44] ARE ONE SCAN BECAUSE THE STANDARD MAKES THEM ONE PRODUCTION WITH TWO ENDINGS. Character for
 * character the two are identical up to the final delimiter, they carry the SAME well-formedness constraint
 * ([WFC: Unique Att Spec] is written on both), and §3.1's own prose introduces the second as "a special form"
 * of the first. Two scans would be one grammar transcribed twice, which is how two spellings drift apart; the
 * ending each one found is a FACT the scan reports (`XmlTag.empty`) and not a choice a caller makes before it
 * runs. What that fact means — that [39] `element ::= EmptyElemTag | STag content ETag` took its first
 * alternative, so no [43] content and no [42] ETag follow — is §3's sentence and belongs to whoever walks [39].
 *
 * [WFC: Element Type Match] IS DELIBERATELY NOT CHECKED HERE, and the reason is a section number. "The Name in
 * an element's end-tag MUST match the element type in the start-tag" is written on [39] `element` in §3
 * Logical Structures, not on [42] `ETag` in §3.1 — because it is a statement about a PAIR of tags, and this
 * component sees one tag at a time. A version of it asserted here would need the scan to be told the open
 * element's name, which is a caller inventing a parameter for a constraint that is not this file's; the [39]
 * walk holds the stack, so the [39] walk owns the match.
 *
 * THIS IS THE FIRST COMPONENT HERE THAT ALLOCATES, AND BOTH HALVES OF THAT ARE THE STANDARD'S DOING.
 * (a) [40]'s `(S Attribute)*` is a Kleene star, so the attribute list is bounded by the ENTITY and by nothing
 * this component decides — CLAUDE.md's no-bounds rule, which a "maximum attributes per element" would violate
 * while looking like prudence. (b) §3.3.3 PRODUCES text rather than selecting a slice of it: `&lt;` is four
 * characters of the entity and one character of the value, a #xD#xA is two and one, and a tab that came from
 * `&#x9;` survives while a literal tab does not. So an attribute value CANNOT be a borrowed slice the way
 * every content run in core/xml/xml_markup.h is, and the difference is not an optimization anyone may take
 * back later. NAMES STILL BORROW, for core/xml/xml_ref.h's exact reason: §2.11 only ever rewrites #xD, and #xD
 * is in neither §2.3's [4] `NameStartChar` nor its [4a] `NameChar`, so no character of a Name can differ from
 * the bytes it was decoded from.
 *
 * THE VALUE BUFFER IS SIZED ONCE, EXACTLY, BEFORE A CHARACTER OF IT IS READ, because §3.3.3 can only ever
 * SHRINK. Every alternative of [10] is at least as long in bytes as what it contributes: a literal character
 * re-encodes to the bytes it was decoded from, a white space character becomes one byte from one or two, and
 * every reference is longer than its result — §4.6's shortest is `&lt;` at four bytes for one, and a [66]
 * `CharRef` needs three decimal digits before it can name a code point that takes two bytes in UTF-8, four
 * before three, five before four. So the run between the quotes is an upper bound on the normalized value,
 * the closing quote is found by a single byte scan before the character scan begins, and the fill is asserted
 * against that bound rather than trusted to respect it. THE BYTE SCAN FOR THE QUOTE IS EXACT for
 * core/xml/xml_name.h's colon argument — `"` and `'` are ASCII, so neither can occur as a CONTINUATION byte of
 * some other code point — and that it finds the CLOSING quote rather than some interior one is [10]'s own
 * grammar: the delimiter is excluded from the character alternative, and a [67] Reference is made of Name
 * characters or digits, among which no quote stands. That is asserted at the end of every value scan rather
 * than argued once here.
 *
 * §3.3.3 STEP 4 IS UNREACHABLE IN THIS BUILD AND THAT IS THE STANDARD'S ANSWER, NOT A STUB. Step 4 — discard
 * leading and trailing #x20 and collapse runs — applies only when "the attribute type is not CDATA", which is
 * a fact from a §3.3 attribute-list declaration, which stands only inside §2.8's [28] `doctypedecl`'s [28b]
 * `intSubset` or in an external subset. Nothing in this build reads either — core/xml/xml_doctype.h crashes at
 * the `'['` and no external subset is ever dereferenced — so no declaration can have been read, and §3.3.3
 * states what a processor does then in its own words: "All attributes for which no declaration has been read
 * SHOULD be treated by a non-validating
 * processor as if declared CDATA." The CDATA arm is therefore the only arm this build can reach rather than
 * the easy one it took. When §3.3 exists, step 4 is applied BY WHOEVER KNOWS THE TYPE, over the value this
 * component produced — it is a second pass over a finished string and needs nothing from the scan.
 *
 * THE ONLY ENTITIES THAT RESOLVE ARE §4.6'S FIVE, AND THAT CLOSES THE EXTERNAL-ENTITY SURFACE AT THE GRAMMAR
 * RATHER THAN BY A POLICY. An [68] `EntityRef` whose Name is not one of amp, lt, gt, apos, quot is answered
 * XML_TAG_ERR_ENTITY_UNDECLARED — "the Name given in the entity reference MUST match that in an entity
 * declaration ... except that well-formed documents need not declare any of the following entities: amp, lt,
 * gt, apos, quot" — and no declaration is ever read here, so the five are the whole of what resolves. No code
 * path can therefore turn an entity into a SystemLiteral, which means no attribute value can cause a fetch:
 * the classic external-entity read, closed because there is nothing to widen rather than because something
 * narrowed it. WHETHER THAT ANSWER IS THE CONSTRAINT'S IS NOT THIS COMPONENT'S QUESTION — [WFC: Entity
 * Declared]'s three clauses turn on facts about the DOCUMENT (§2.8's [28] `ExternalID` and §2.9's [32]
 * `SDDecl`) that no tag can see, and core/xml/xml_document.h holds both and decides. When §4.2's [70]
 * `EntityDecl` is built, the two sentences that must be built WITH it are §3.1's [WFC: No External Entity
 * References] ("Attribute values MUST NOT contain direct or indirect entity references to external entities")
 * and §4.4.4 Forbidden's third bullet ("a reference to an external entity in an attribute value"), which make
 * it a FATAL ERROR and not a configuration choice.
 *
 * §4.6'S DOUBLE ESCAPING IS WHY [WFC: No < in Attribute Values] IS FULLY DECIDED HERE. §4.6 requires that "if
 * the entities lt or amp are declared, they MUST be declared as internal entities whose replacement text is a
 * character reference to the respective character ... the double escaping is REQUIRED for these entities so
 * that references to them produce a well-formed result". So `&lt;` in an attribute value resolves through a
 * [66] CharRef and its replacement text holds no literal `<` — the constraint is about a LITERAL `<` reaching
 * the value, which [10]'s own character class already excludes and which this scan reports as
 * XML_TAG_ERR_LT_IN_ATTRIBUTE_VALUE. With no other entity resolvable, no replacement text exists that could
 * carry one indirectly, so the constraint is not partly checked here: it is closed.
 *
 * [WFC: Unique Att Spec] IS BY THE LITERAL Name, WHICH IS NOT THE WHOLE OF UNIQUENESS AND IS THE WHOLE OF THIS
 * SECTION'S. XML §3.1 "Start-Tags, End-Tags, and Empty-Element Tags" says "An attribute name MUST NOT appear
 * more than once in the same start-tag or empty-element tag", and a Name is a byte run, so a byte comparison
 * decides it exactly. Namespaces in XML 1.0
 * (Third Edition) §6.3 Uniqueness of Attributes adds a SECOND sentence — "Namespace constraint: Attributes
 * Unique" also forbids two attributes "with qualified names with the same local part and with prefixes which
 * have been bound to namespace names that are identical" — and that one cannot be answered here at all,
 * because binding a prefix needs the scope core/xml/xml_ns.h holds and the declarations are attributes of THIS
 * tag, so they are not in scope until the tag is finished. It belongs to whoever pushes the scope.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED SCAN CONSUMES NOTHING — for
 * core/xml/xml_markup.h's reasons, which are that a malformed document is a page's INPUT and that the position
 * a report quotes must name the CONSTRUCT rather than some place inside the one that failed. THE ONE CARVE-OUT
 * IS THE SAME ONE: an error the character layer latched is NOT rewound, because restoring a saved reader would
 * restore its §1.2 latch to XML_CHAR_OK and silently un-report the fatal error that layer just detected. */
#ifndef APICLIENT_XML_TAG_H
#define APICLIENT_XML_TAG_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"
#include "core/xml/xml_ref.h"

/* WHICH SENTENCE OF THE STANDARD THIS TAG VIOLATED. One value per sentence, for core/xml/xml_ns.h's reason: a
   tag that names nothing, an attribute with no `=`, a value nobody closed, a duplicated attribute name and a
   reference to an entity nobody declared are five different mistakes an author has to be told apart. Zero is
   OK so a caller may write `if (err)`. */
typedef enum {
    XML_TAG_OK = 0,
    XML_TAG_ERR_NAME,                    /* [40]/[42]/[44]: `<` or `</` is not followed by a [5] Name */
    XML_TAG_ERR_ATTRIBUTE_SEPARATOR,     /* [40]/[44]: `(S Attribute)*` — an Attribute with no [3] S before it */
    XML_TAG_ERR_ATTRIBUTE_NAME,          /* [41]: the attribute specification does not begin with a [5] Name */
    XML_TAG_ERR_EQ,                      /* [25] Eq ::= S? '=' S? — the `=` is not there */
    XML_TAG_ERR_ATTVALUE_QUOTE,          /* [10]: the value is opened by neither `"` nor `'` */
    XML_TAG_ERR_ATTVALUE_UNTERMINATED,   /* [10]: the entity ends before the value's own delimiter */
    XML_TAG_ERR_LT_IN_ATTRIBUTE_VALUE,   /* [10]'s charset and §3.1's [WFC: No < in Attribute Values] */
    XML_TAG_ERR_UNIQUE_ATT_SPEC,         /* §3.1 [WFC: Unique Att Spec] */
    XML_TAG_ERR_ENTITY_UNDECLARED,       /* §4.1 [WFC: Entity Declared] — see the head comment */
    XML_TAG_ERR_UNTERMINATED,            /* [40]/[42]/[44]: the entity ends before the tag does */
    XML_TAG_ERR_ETAG_ATTRIBUTE,          /* [42]: an end-tag carries no attribute specifications */
    XML_TAG_ERR_REFERENCE,               /* §4.1's layer reported one — the `ref` out-parameter names which */
    XML_TAG_ERR_CHARACTER                /* the character layer latched one — ask xml_char_error_message(r->fatal) */
} XmlTagError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_TAG_OK has a message
   too, and a caller that formats it has asked the wrong question. XML_TAG_ERR_REFERENCE's and
   XML_TAG_ERR_CHARACTER's messages say to ask the layer below, because those layers' sentences are THEIRS to
   word and duplicating them here is how two spellings drift apart. */
const char *xml_tag_error_message(XmlTagError err);

/* ONE [41] `Attribute`, AFTER §3.3.3. The name BORROWS bytes of the entity the reader was initialised over and
   the value is OWNED — see the head comment for why the two differ and why neither may become the other.
   `value` is never NULL, is NUL-terminated for the convenience of whatever materializes it, and holds no
   interior NUL: U+0000 is not §2.2's [2] Char, so no character of an XML document is one and the terminator is
   unambiguous. That is asserted rather than assumed. */
typedef struct {
    const char *name;  size_t name_len;
    char       *value; size_t value_len;
} XmlAttribute;

/* ONE [40] `STag` OR [44] `EmptyElemTag`. `name` is the element type, borrowed. `atts` is owned and is NULL
   exactly when `att_n` is zero — a tag with no attributes has no array, which is a positive statement and not
   a hole, and a caller iterating `att_n` never reads it. `empty` records WHICH of the two productions matched;
   §3's [39] is what that fact is for. Zero-initialise before the first scan and `xml_tag_free` after the last
   use; a scan that returns anything but XML_TAG_OK writes NOTHING, so a failed scan leaves whatever was there
   and never leaks a half-built list. */
typedef struct {
    const char   *name;  size_t name_len;
    XmlAttribute *atts;  size_t att_n;
    bool          empty;
} XmlTag;

/* Release what a successful scan allocated and leave the record zeroed, so a second free is a no-op rather
   than a double free and a caller may free a record no scan ever filled. */
void xml_tag_free(XmlTag *t);

/* Does the reader stand at [40]/[44]'s `<`, or at [42]'s `</`? Both are byte compares and both are exact:
   `<`, `/`, `!` and `?` are ASCII, so none can occur as a continuation byte of some other code point, and the
   four constructs a `<` can open here — a tag, an end-tag, §2.5/§2.7's `<!` forms and §2.6/§2.8's `<?` forms —
   are told apart by the single byte after it. A `<` followed by something that is not a [4] NameStartChar
   still answers TRUE to the first: the peek ROUTES and the scan DECIDES, which is what makes
   XML_TAG_ERR_NAME a sentence about the document rather than a shape the caller had to recognise first. */
bool xml_tag_at_stag(const XmlCharReader *r);
bool xml_tag_at_etag(const XmlCharReader *r);

/* SCAN ONE [40] `STag` or [44] `EmptyElemTag`. The reader MUST stand where `xml_tag_at_stag` answers true —
   the caller has peeked, and a reader standing anywhere else is a caller that has not, which is a DCHECK and
   not an error to report about the document.
   `*out` is written ONLY when XML_TAG_OK is returned, and `*ref` is written ALWAYS: it is XML_REF_OK unless
   the answer is XML_TAG_ERR_REFERENCE, which is a positive statement that §4.1's layer found nothing to
   report and not a field defaulted past. It is an out-parameter rather than a value folded into XmlTagError
   because the alternative is transcribing core/xml/xml_ref.h's five sentences into this enum, and one rule
   written in two places is the defect this component set is built to avoid.
   On any answer but XML_TAG_OK the reader is byte-for-byte the one that was handed in, except for
   XML_TAG_ERR_CHARACTER — see the head comment. */
XmlTagError xml_tag_scan_stag(XmlCharReader *r, XmlTag *out, XmlRefError *ref);

/* SCAN ONE [42] `ETag ::= '</' Name S? '>'`. The reader MUST stand where `xml_tag_at_etag` answers true.
   No attributes, no [10] AttValue and therefore no [67] Reference stands in an end-tag, so there is nothing to
   normalize, nothing to allocate and no reference error to report — which is why this answers with a borrowed
   Name and not an XmlTag. `*name`/`*name_len` are written ONLY when XML_TAG_OK is returned. */
XmlTagError xml_tag_scan_etag(XmlCharReader *r, const char **name, size_t *name_len);

#endif
