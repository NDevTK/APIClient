/* XML 1.0 (Fifth Edition) §2.3 Common Syntactic Constructs' [11] `SystemLiteral`, [12] `PubidLiteral` and
 * [13] `PubidChar` — THE TWO QUOTED STRINGS AN EXTERNAL IDENTIFIER IS MADE OF.
 *
 * WHAT IT IS. §2.3 groups the standard's quoted strings under one heading — "Literal data is any quoted string
 * not containing the quotation mark used as a delimiter for that string. Literals are used for specifying the
 * content of internal entities (EntityValue), the values of attributes (AttValue), and external identifiers
 * (SystemLiteral)" — and this file is the EXTERNAL IDENTIFIER half of that group: [11] `SystemLiteral ::=
 * ('"' [^"]* '"') | ("'" [^']* "'")`, [12] `PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"`
 * and [13] `PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] | [-'()+,./:=?;!*#@$_%]`. It sits on
 * core/xml/xml_char.h and on NOTHING ELSE.
 *
 * WHY THESE TWO AND NOT §2.3's OTHER TWO, which is a fact about the GRAMMAR rather than a partition of
 * convenience. §2.3's own note says it: "Note that a SystemLiteral can be parsed without scanning for markup."
 * [11] and [12] are pure character runs whose only terminator is the delimiter — §2.8 Prolog and Document Type
 * Declaration states from the other side that "Parameter entity references are recognized anywhere in the DTD
 * (internal and external subsets and external parameter entities), except in literals, processing
 * instructions, comments", and §4.4 XML Processor Treatment of Entities and References' table names
 * SystemLiteral and PubidLiteral among the contexts its "Reference in DTD" row is defined OUTSIDE of. So a `&`
 * or a `%` here is character data and nothing more, which is exactly core/xml/xml_markup.h's situation and
 * exactly not [9] `EntityValue`'s or [10] `AttValue`'s: those two have `PEReference` and `Reference` inside
 * their own alternations, so scanning one means resolving references, which means §4.2 Entity Declarations and
 * §4.5 Construction of Entity Replacement Text. Those belong with the declarations they are components of; a
 * file that scanned all four literals would be two problems wearing one heading.
 *
 * THE `- "'"` IN [12] IS REDUNDANT TO A SCANNER AND IS STILL THE POINT OF THE PRODUCTION. [12]'s two
 * alternatives are NOT symmetrical: with the quotation-mark delimiter the content is `PubidChar*`, which
 * INCLUDES the apostrophe because [13]'s punctuation set has one; with the apostrophe delimiter the content is
 * `(PubidChar - "'")*`, which does not. A scanner that breaks on the delimiter gets the subtraction for free —
 * an apostrophe cannot be content of an apostrophe-delimited literal because it ends it — but the two
 * alternatives are what make the language unambiguous, and the asymmetry is REAL and observable: `"O'Reilly"`
 * is a public identifier and `'O'Reilly'` is not the same one, it is the public identifier `O` followed by
 * characters no production accepts. The quotation mark is in neither alternative, because [13]'s set has no
 * `"` in it at all.
 *
 * WHAT COMES BACK IS A BORROWED SLICE OF THE ENTITY, NOT THE CHARACTERS, and the difference is exactly §2.11
 * End-of-Line Handling — core/xml/xml_markup.h's arrangement, for its reasons. A literal's content is a run of
 * characters and a character is what core/xml/xml_char.h PRODUCES, so the bytes `a` #xD #xA `b` inside either
 * literal are the three characters `a` #xA `b`. A caller that must materialize the characters asks
 * core/xml/xml_char.h for §2.11's rule over the slice; every scan here holds the two spellings of §2.11 to
 * each other on every call rather than trusting them to agree.
 *   [13] LISTS #xD AND NO LITERAL CAN EVER CONTAIN ONE, which is not a contradiction but the same note §2.3
 * makes about [3] `S`: "all #xD characters literally present in an XML document are either removed or replaced
 * by #xA characters before any other processing is done. The only way to get a #xD character to match this
 * production is to use a character reference" — and a character reference is not recognized in these literals
 * at all. So the class is transcribed complete, and the character that would exercise its #xD row is
 * unreachable from here. Transcribing it short would be a mis-transcription of a normative production for the
 * sake of a case the layer below already handles.
 *
 * THE `#` RULE OF §4.2.2 IS AN `error` AND NOT A `fatal error`, AND THAT DECIDES WHERE IT LIVES. §4.2.2
 * External Entities: "It is an error for a fragment identifier (beginning with a # character) to be part of a
 * system identifier." §1.2 Terminology defines `error` as "A violation of the rules of this specification;
 * results are undefined ... Conforming software MAY detect and report an error and MAY recover from it", while
 * a `fatal error` is one a processor "MUST detect and report" and after which it "MUST NOT continue normal
 * processing". Those are different obligations, so reporting the `#` as a scan failure here would END a parse
 * the standard permits to continue — and a `#` is a perfectly good character of [11]'s `[^"]*` either way. It
 * belongs where §4.2.2 puts it, at the point the system identifier "is meant to be converted to a URI
 * reference ... as part of the process of dereferencing it", which is the step that decides whether to
 * dereference at all.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED SCAN CONSUMES NOTHING — both for
 * core/xml/xml_ref.h's reasons, with the same single carve-out: an error the character layer latched is NOT
 * rewound, because restoring a saved reader would restore its §1.2 latch to XML_CHAR_OK and silently
 * un-report the fatal error that layer just detected. */
#ifndef APICLIENT_XML_LITERAL_H
#define APICLIENT_XML_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/xml/xml_char.h"

/* WHICH SENTENCE OF THE STANDARD THIS LITERAL VIOLATED. One value per sentence, for core/xml/xml_ref.h's
   reason. A literal nobody closed and one holding a character [13] does not have are two different places to
   look. Zero is OK so a caller may write `if (err)`.
     THERE IS NO `the literal never opened` ANSWER, and that is the peek rule rather than an omission: the
   opening delimiter is what `xml_literal_at` reports and a scan handed a reader standing anywhere else is a
   caller that has not peeked, which is this engine's mistake and not the document's — so it is the DCHECK
   below and not a value here. A value the component can never return is a field nobody would notice was never
   written. */
typedef enum {
    XML_LITERAL_OK = 0,
    XML_LITERAL_ERR_UNTERMINATED,   /* [11]/[12]: the delimiter that opened the literal does not close it */
    XML_LITERAL_ERR_PUBID_CHAR,     /* [13]: a character the public identifier class does not have */
    XML_LITERAL_ERR_CHARACTER       /* the character layer latched one — ask xml_char_error_message */
} XmlLiteralError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_LITERAL_OK has a
   message too, and a caller that formats it has asked the wrong question. XML_LITERAL_ERR_CHARACTER's message
   says to ask the reader, because §2.2's and §4.3.3's character sentences are core/xml/xml_char.h's to word
   and a second copy here is how two spellings drift apart. */
const char *xml_literal_error_message(XmlLiteralError err);

/* §2.3's [13] `PubidChar`, ONE CODE POINT AT A TIME — exposed rather than private because it is a NAMED
   production of the standard and because the fixture that holds its transcription to the spec has to be able
   to sweep it, which is the only way a hand-copied punctuation set is checked at all. See the head comment
   for why #xD is in it and unreachable through it. */
bool xml_literal_is_pubid_char(uint32_t cp);

/* DOES THE READER STAND AT A LITERAL'S OPENING DELIMITER? THE PEEK IS THE CALLER'S AND THE SCAN ASSERTS IT,
   which is core/xml/xml_markup.h's arrangement: §4.2.2's [75] `ExternalID ::= 'SYSTEM' S SystemLiteral |
   'PUBLIC' S PubidLiteral S SystemLiteral` is what decides WHICH literal stands here, and that is its grammar
   rule rather than this component's — the two productions are told apart by the keyword before them and by
   nothing inside them.
     ONE predicate serves both because both open the same way. It is a byte compare and that is exact, by
   core/xml/xml_name.h's argument for the colon: an apostrophe and a quotation mark are ASCII, so each is a
   single byte in UTF-8, and neither byte can occur as a CONTINUATION byte of some other code point because
   every continuation byte is 0x80..0xBF. */
bool xml_literal_at(const XmlCharReader *r);

/* SCAN ONE LITERAL. The reader MUST stand where `xml_literal_at` is true, and standing anywhere else is a
   caller that has not peeked, which is a DCHECK and not a document to report about.
   `*raw`/`*raw_len` are written ONLY when XML_LITERAL_OK is returned, and the reader is then positioned
   immediately after the closing delimiter. `*raw` is never NULL, including for an empty run: `""` and `''` are
   a `SystemLiteral` of zero characters standing AT a position in the entity, which is not the same fact as
   having no literal at all — and §4.2.2 has no minimum length for a system identifier.
   On any other answer neither out-parameter is touched and — except for XML_LITERAL_ERR_CHARACTER, see the
   head comment — the reader is byte-for-byte the one that was handed in. Two out-parameters rather than a
   struct because [75]'s caller holds a PAIR of these literals and fills its own record's four fields from
   them; a one-field-per-literal wrapper would be a type per production with no behavior in it. */
XmlLiteralError xml_literal_scan_system(XmlCharReader *r, const char **raw, size_t *raw_len);
XmlLiteralError xml_literal_scan_pubid(XmlCharReader *r, const char **raw, size_t *raw_len);

#endif
