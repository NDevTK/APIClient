/* XML 1.0 (Fifth Edition) §2.7 CDATA Sections — ONE OF §2.4 Character Data and Markup'S MARKUP FORMS, AND THE
 * ONE IN WHICH NO OTHER MARKUP IS RECOGNIZED.
 *
 * WHAT IT IS. Four productions and one sentence. §2.7's [18] `CDSect ::= CDStart CData CDEnd`, [19] `CDStart
 * ::= '<![CDATA['`, [20] `CData ::= (Char* - (Char* ']]>' Char*))`, [21] `CDEnd ::= ']]>'`, and the sentence
 * that decides the whole design: "Within a CDATA section, only the CDEnd string is recognized as markup, so
 * that left angle brackets and ampersands may occur in their literal form; they need not (and cannot) be
 * escaped using "&lt;" and "&amp;"." It sits directly on core/xml/xml_char.h and on NOTHING ELSE — in
 * particular NOT on core/xml/xml_ref.h, which is the point of the file rather than an omission from it.
 *
 * THE REFERENCE LAYER IS NOT REACHED FROM HERE, AND "CANNOT" IS THE WORD THAT MAKES THAT A RULE. §2.4 states
 * the general prohibition — "The ampersand character (&) and the left angle bracket (<) MUST NOT appear in
 * their literal form, except when used as markup delimiters, or within a comment, a processing instruction, or
 * a CDATA section" — and §2.7 names the exception's consequence in both directions: an `&` inside a CDATA
 * section not only NEED NOT be escaped, it CANNOT be, because `&amp;` there is five character-data characters
 * and not an ampersand. So a scan that ran §4.1's [67] `Reference` over this content would not be a stricter
 * parser, it would be a WRONG one: `<![CDATA[&amp;]]>` would yield `&` where the standard yields `&amp;`, and
 * `<![CDATA[&foo;]]>` would report a [WFC: Entity Declared] fatal error about a document that is well-formed.
 * This is also the sentence that decides what a `<script>` body is in an XHTML document: HTML §14.2 "Parsing
 * XML documents" runs an XML parser, which has no raw-text tokenizer state, so `<script><![CDATA[ … ]]></script>`
 * is §2.7's construct and its content is the script's program text as character data.
 *
 * WHAT COMES BACK IS A BORROWED SLICE OF THE ENTITY, NOT THE CHARACTERS, and the difference is exactly §2.11
 * End-of-Line Handling. §2.7's content is `Char*`, and a Char is what core/xml/xml_char.h PRODUCES — so the
 * bytes `a` #xD #xA `b` inside a CDATA section are the three characters `a` #xA `b`. The slice is the run the
 * scan consumed, which is the run a caller wants when — as is overwhelmingly usual — no #xD stands in it; a
 * caller that must materialize the CHARACTERS asks core/xml/xml_char.h for §2.11's rule over the slice, which
 * is that file's sentence to own and is why this one does not restate it. The scan holds the two spellings of
 * §2.11 to each other on every call rather than trusting them to agree.
 *
 * NO ALLOCATION, AND THE SCAN'S WHOLE STATE IS ITS READER PLUS A TWO-CHARACTER LOOKAHEAD. [20]'s `Char*` puts
 * no bound on the content, so the run is bounded by the ENTITY and by nothing this component decides —
 * CLAUDE.md's no-bounds rule, which a "maximum CDATA length" would violate while looking like prudence. What
 * makes that affordable is that nothing here grows: the answer is a pair of pointers into bytes the caller
 * already owns, and the reader is the POD position core/xml/xml_char.h documents, so a copy is both the peek
 * and the park.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, and a FAILED SCAN CONSUMES NOTHING — both for
 * core/xml/xml_ref.h's reasons, which are that a malformed document is a page's INPUT (§2.1 Well-Formed XML
 * Documents makes matching the grammar the first condition of well-formedness and §1.2 Terminology makes a
 * violated well-formedness constraint a fatal error) and that the position a report quotes must name the
 * CONSTRUCT rather than some place inside the one that failed. THE ONE CARVE-OUT IS THE SAME ONE: an error the
 * character layer latched is NOT rewound, because restoring a saved reader would restore its §1.2 latch to
 * XML_CHAR_OK and silently un-report the fatal error that layer just detected. */
#ifndef APICLIENT_XML_MARKUP_H
#define APICLIENT_XML_MARKUP_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"

/* WHICH SENTENCE OF THE STANDARD THIS CONSTRUCT VIOLATED. One value per sentence, for core/xml/xml_ref.h's
   reason: an author has to be told which mistake was made, and a report that merged two would send them to the
   wrong one. Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_MARKUP_OK = 0,
    XML_MARKUP_ERR_UNTERMINATED_CDATA_SECTION,   /* [18]: the [21] CDEnd that closes the section is not there */
    XML_MARKUP_ERR_CHARACTER                     /* the character layer latched one — ask xml_char_error_message */
} XmlMarkupError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_MARKUP_OK has a
   message too, and a caller that formats it has asked the wrong question. XML_MARKUP_ERR_CHARACTER's message
   says to ask the reader, because §2.2's and §4.3.3's sentences are core/xml/xml_char.h's to word and a second
   copy here is how two spellings drift apart. */
const char *xml_markup_error_message(XmlMarkupError err);

/* THE CONTENT OF A CONSTRUCT, BORROWED FROM THE ENTITY AND NOT YET §2.11-NORMALIZED — see the head comment.
   `raw` is never NULL, including for an empty run: `<![CDATA[]]>` has a content run of zero bytes AT a position
   in the entity, which is not the same fact as having no content at all. */
typedef struct {
    const char *raw;
    size_t      raw_len;
} XmlMarkupText;

/* DOES THE READER STAND AT [19] `CDStart`? THE PEEK IS THE CALLER'S AND THE SCAN ASSERTS IT, which is
   core/xml/xml_ref.h's arrangement one delimiter longer: a caller in [43] `content` that has seen a `<` must
   decide between a CDATA section, a comment, a processing instruction, a start-tag and an end-tag, and that
   decision is ITS grammar rule and not this component's. Exposing the peek is what keeps [19]'s nine
   characters written down ONCE — a caller that spelled the delimiter itself would be the same fact in two
   places, and the two would drift.
     IT IS A BYTE COMPARE AND THAT IS EXACT, by core/xml/xml_name.h's argument for the colon: every character
   of [19] is ASCII, so each is a single byte in UTF-8, and none of those bytes can occur as a CONTINUATION
   byte of some other code point because every continuation byte is 0x80..0xBF. So "are the next nine
   characters `<![CDATA[`" and "are the next nine bytes those" are the same question. §1.2 Terminology's `match`
   performs no case folding, so `<![cdata[` is not this delimiter and this predicate says so. */
bool xml_markup_at_cdsect(const XmlCharReader *r);

/* SCAN ONE §2.7 [18] `CDSect`. The reader MUST stand at [19] CDStart — see the predicate above — and standing
   anywhere else is a caller that has not peeked, which is a DCHECK and not a document to report about.
   `*out` is written ONLY when XML_MARKUP_OK is returned, and the reader is then positioned immediately after
   [21] CDEnd. On any other answer `*out` is untouched and — except for XML_MARKUP_ERR_CHARACTER, see the head
   comment — the reader is byte-for-byte the one that was handed in. */
XmlMarkupError xml_markup_scan_cdsect(XmlCharReader *r, XmlMarkupText *out);

#endif
