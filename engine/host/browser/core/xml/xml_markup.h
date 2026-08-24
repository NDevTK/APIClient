/* XML 1.0 (Fifth Edition) §2.5 Comments, §2.6 Processing Instructions and §2.7 CDATA Sections — THE THREE
 * MARKUP FORMS §2.4 Character Data and Markup NAMES AS THE PLACES A LITERAL `<` OR `&` MAY STAND.
 *
 * WHAT IT IS, AND WHY THE THREE ARE ONE FILE. §2.4 states the general prohibition and its exception in one
 * sentence — "The ampersand character (&) and the left angle bracket (<) MUST NOT appear in their literal
 * form, except when used as markup delimiters, or within a comment, a processing instruction, or a CDATA
 * section" — so the grouping is the standard's own and not a convenience. All three are the same SHAPE: a
 * literal opening delimiter, a run of §2.2 [2] `Char` restricted only by a forbidden literal string, and a
 * literal closing delimiter. §2.5's [15] `Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'`,
 * §2.6's [16] `PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'` with [17] `PITarget ::= Name -
 * (('X' | 'x') ('M' | 'm') ('L' | 'l'))`, and §2.7's [18] `CDSect ::= CDStart CData CDEnd` with [19] `CDStart
 * ::= '<![CDATA['`, [20] `CData ::= (Char* - (Char* ']]>' Char*))` and [21] `CDEnd ::= ']]>'`. It sits on
 * core/xml/xml_char.h, and on core/xml/xml_name.h for [17]'s `Name` — and on NOTHING ELSE.
 *
 * §4.1'S REFERENCE LAYER IS NOT REACHED FROM HERE, AND THAT IS THE POINT OF THE FILE RATHER THAN AN OMISSION
 * FROM IT. §2.7 names the exception's consequence in both directions: an `&` inside a CDATA section not only
 * NEED NOT be escaped, it CANNOT be, because `&amp;` there is five characters of character data and not an
 * ampersand. So a scan that ran §4.1's [67] `Reference` over any of these three would not be a stricter
 * parser, it would be a WRONG one: `<![CDATA[&amp;]]>` would yield one character where the standard yields
 * five, and `<![CDATA[&foo;]]>` would report a [WFC: Entity Declared] fatal error about a document that is
 * well-formed. §2.5 and §2.6 say the same of themselves from the other side — "Parameter entity references
 * MUST NOT be recognized within comments" and "... within processing instructions" — and §2.4's own sentence
 * covers the general ones.
 *   This is also what decides what a `<script>` body is in an XHTML document: HTML §14.2 "Parsing XML
 * documents" runs an XML parser, which has no raw-text tokenizer state, so `<script><![CDATA[ … ]]></script>`
 * is §2.7's construct and its content is the program text as character data.
 *
 * WHAT COMES BACK IS A BORROWED SLICE OF THE ENTITY, NOT THE CHARACTERS, and the difference is exactly §2.11
 * End-of-Line Handling. Each content run is `Char*`, and a Char is what core/xml/xml_char.h PRODUCES — so the
 * bytes `a` #xD #xA `b` inside any of these three are the three characters `a` #xA `b`. The slice is the run
 * the scan consumed, which is what a caller wants when — as is overwhelmingly usual — no #xD stands in it; a
 * caller that must materialize the CHARACTERS asks core/xml/xml_char.h for §2.11's rule over the slice, which
 * is that file's sentence to own and is why this one does not restate it. Every scan holds the two spellings
 * of §2.11 to each other on every call rather than trusting them to agree.
 *   [17]'s `PITarget` is the exception and it is exact rather than convenient: §2.11 only ever rewrites #xD,
 * and #xD is in neither §2.3's [4] `NameStartChar` nor its [4a] `NameChar`, so no character of a target can
 * differ from the bytes it was decoded from — the same argument core/xml/xml_ref.h makes for [68]'s Name.
 *
 * NO ALLOCATION, AND A SCAN'S WHOLE STATE IS ITS READER PLUS A ONE-CHARACTER LOOKAHEAD. None of these
 * productions bounds its content, so a run is bounded by the ENTITY and by nothing this component decides —
 * CLAUDE.md's no-bounds rule, which a "maximum comment length" would violate while looking like prudence.
 * What makes that affordable is that nothing here grows: an answer is a pair of pointers into bytes the
 * caller already owns, and the reader is the POD position core/xml/xml_char.h documents, so a copy is both
 * the peek and the park.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, and a FAILED SCAN CONSUMES NOTHING — both for
 * core/xml/xml_ref.h's reasons, which are that a malformed document is a page's INPUT (§2.1 Well-Formed XML
 * Documents makes matching the grammar the first condition of well-formedness and §1.2 Terminology makes a
 * violated well-formedness constraint a fatal error) and that the position a report quotes must name the
 * CONSTRUCT rather than some place inside the one that failed. THE ONE CARVE-OUT IS THE SAME ONE: an error
 * the character layer latched is NOT rewound, because restoring a saved reader would restore its §1.2 latch
 * to XML_CHAR_OK and silently un-report the fatal error that layer just detected. */
#ifndef APICLIENT_XML_MARKUP_H
#define APICLIENT_XML_MARKUP_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"

/* WHICH SENTENCE OF THE STANDARD THIS CONSTRUCT VIOLATED. One value per sentence, for core/xml/xml_ref.h's
   reason: an author has to be told which mistake was made, and a report that merged two would send them to
   the wrong one. The three "unterminated" answers are three values and not one for exactly that reason — a
   comment nobody closed, a processing instruction nobody closed and a CDATA section nobody closed are three
   places to look. Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_MARKUP_OK = 0,
    XML_MARKUP_ERR_UNTERMINATED_COMMENT,         /* [15]: the '-->' that closes the comment is not there */
    XML_MARKUP_ERR_DOUBLE_HYPHEN,                /* §2.5: the string "--" MUST NOT occur within comments */
    XML_MARKUP_ERR_PI_TARGET,                    /* [17]: what follows '<?' is not a §2.3 [5] Name */
    XML_MARKUP_ERR_PI_TARGET_RESERVED,           /* [17]: Name minus (('X'|'x')('M'|'m')('L'|'l')) */
    XML_MARKUP_ERR_PI_TARGET_END,                /* [16]: the target is followed by neither [3] S nor '?>' */
    XML_MARKUP_ERR_UNTERMINATED_PI,              /* [16]: the '?>' that closes the instruction is not there */
    XML_MARKUP_ERR_UNTERMINATED_CDATA_SECTION,   /* [18]: the [21] CDEnd that closes the section is not there */
    XML_MARKUP_ERR_CHARACTER                     /* the character layer latched one — ask xml_char_error_message */
} XmlMarkupError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_MARKUP_OK has a
   message too, and a caller that formats it has asked the wrong question. XML_MARKUP_ERR_CHARACTER's message
   says to ask the reader, because §2.2's and §4.3.3's sentences are core/xml/xml_char.h's to word and a
   second copy here is how two spellings drift apart. */
const char *xml_markup_error_message(XmlMarkupError err);

/* THE CONTENT OF A CONSTRUCT, BORROWED FROM THE ENTITY AND NOT YET §2.11-NORMALIZED — see the head comment.
   `raw` is never NULL, including for an empty run: `<![CDATA[]]>`, `<!---->` and `<?t?>` each have a content
   run of zero bytes AT a position in the entity, which is not the same fact as having no content at all. */
typedef struct {
    const char *raw;
    size_t      raw_len;
} XmlMarkupText;

/* §2.6's [16] `PI`, split into the two halves a DOM `ProcessingInstruction` node is built from.
   `target` is [17]'s `PITarget`, borrowed and byte-exact (see the head comment on why no #xD can be in one).
   `data` is the instruction, `raw`-borrowed like every other content run here.
     WHAT IS DELIBERATELY NOT REPORTED is whether [16]'s optional `(S (Char* - …))?` group was PRESENT: `<?t?>`
   and `<?t ?>` differ in the grammar and NOT in anything a consumer can see, because DOM §4.7's
   `ProcessingInstruction` carries `data` and the answer is the empty string either way. A bool nothing reads
   is a field that goes wrong unnoticed; if an XML serializer ever needs the distinction it is one bool away
   and will have a reader on the day it is added. */
typedef struct {
    const char   *target;
    size_t        target_len;
    XmlMarkupText data;
} XmlPi;

/* DOES THE READER STAND AT THIS CONSTRUCT'S OPENING DELIMITER? THE PEEK IS THE CALLER'S AND THE SCAN ASSERTS
   IT, which is core/xml/xml_ref.h's arrangement a delimiter longer: a caller in [43] `content` that has seen
   a `<` must decide between a comment, a processing instruction, a CDATA section, a start-tag and an end-tag,
   and that decision is ITS grammar rule and not this component's. Exposing the peeks is what keeps each
   delimiter written down ONCE — a caller that spelled `<![CDATA[` itself would be the same fact in two
   places, and the two would drift.
     THEY ARE BYTE COMPARES AND THAT IS EXACT, by core/xml/xml_name.h's argument for the colon: every
   character of every one of these delimiters is ASCII, so each is a single byte in UTF-8, and none of those
   bytes can occur as a CONTINUATION byte of some other code point because every continuation byte is
   0x80..0xBF. So "are the next nine characters `<![CDATA[`" and "are the next nine bytes those" are the same
   question. §1.2 Terminology's `match` performs no case folding, so `<![cdata[` is not that delimiter and
   these predicates say so.
     `xml_markup_at_pi` ANSWERS ABOUT `'<?'` AND NOTHING MORE, and a caller in §2.8's prolog owes [23]
   `XMLDecl` its own question first: `<?xml version="1.0"?>` opens with the same two characters and is a
   DIFFERENT production. Reaching the PI scan with one is not undefined — [17] subtracts that target, so the
   answer is XML_MARKUP_ERR_PI_TARGET_RESERVED, which is the correct report anywhere [23] is not permitted. */
bool xml_markup_at_comment(const XmlCharReader *r);
bool xml_markup_at_pi(const XmlCharReader *r);
bool xml_markup_at_cdsect(const XmlCharReader *r);

/* SCAN ONE CONSTRUCT. The reader MUST stand at the matching opening delimiter — see the predicates above —
   and standing anywhere else is a caller that has not peeked, which is a DCHECK and not a document to report
   about. `*out` is written ONLY when XML_MARKUP_OK is returned, and the reader is then positioned immediately
   after the closing delimiter. On any other answer `*out` is untouched and — except for
   XML_MARKUP_ERR_CHARACTER, see the head comment — the reader is byte-for-byte the one that was handed in. */
XmlMarkupError xml_markup_scan_comment(XmlCharReader *r, XmlMarkupText *out);
XmlMarkupError xml_markup_scan_pi(XmlCharReader *r, XmlPi *out);
XmlMarkupError xml_markup_scan_cdsect(XmlCharReader *r, XmlMarkupText *out);

#endif
