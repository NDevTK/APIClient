/* XML 1.0 (Fifth Edition) §2.8 Prolog and Document Type Declaration's [23] `XMLDecl`, §2.9 Standalone Document
 * Declaration's [32] `SDDecl`, §4.3.1 The Text Declaration's [77] `TextDecl` and §4.3.3 Character Encoding in
 * Entities' [80] `EncodingDecl` — THE DECLARATION AT THE HEAD OF AN ENTITY.
 *
 * WHAT IT IS. Two productions that are one construct, plus the three components they are built from.
 * §2.8's [23] `XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'` opens the document entity;
 * §4.3.1's [77] `TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'` opens an external parsed entity. They
 * differ ONLY in which components are required and in the fact that [77] has no `SDDecl` at all, which is why
 * they are one scan asked two ways rather than two scans: a second copy would be the same grammar written
 * twice, and the two spellings would answer §2.9 differently on the day one of them was edited.
 *   The components are §2.8's [24] `VersionInfo ::= S 'version' Eq ("'" VersionNum "'" | '"' VersionNum '"')`
 * with [25] `Eq ::= S? '=' S?` and [26] `VersionNum ::= '1.' [0-9]+`; §4.3.3's [80] `EncodingDecl ::= S
 * 'encoding' Eq ('"' EncName '"' | "'" EncName "'" )` with [81] `EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*`;
 * and §2.9's [32] `SDDecl ::= S 'standalone' Eq (("'" ('yes' | 'no') "'") | ('"' ('yes' | 'no') '"'))`. It sits
 * on core/xml/xml_char.h and on NOTHING ELSE — every terminal in all three components is ASCII, and the one
 * character class that is not a literal is a digit or a Latin letter.
 *
 * WHY IT IS THE FIRST PIECE OF §2.8 AND NOT [28] doctypedecl. §2.8 writes [22] `prolog ::= XMLDecl? Misc*
 * (doctypedecl Misc*)?`, so the declaration is the FIRST production of the first production of §2.1's
 * [1] `document ::= prolog element Misc*`. And it is what §2.9 is a component OF: an `SDDecl` exists nowhere
 * else in the grammar, so `standalone` cannot be answered — and therefore §4.1's `[WFC: Entity Declared]`
 * cannot be DECIDED — until [23] is scanned. That constraint reads "In a document without any DTD, a document
 * with only an internal DTD subset which contains no parameter entity references, or a document with
 * `standalone='yes'` ... the Name given in the entity reference MUST match that in an entity declaration",
 * so its three alternatives are two facts about §2.8's [28] and one fact about §2.9's [32], and this file
 * supplies the third.
 *
 * THE PEEK IS `'<?xml'` FOLLOWED BY [3] `S`, AND THE `S` IS THE WHOLE DISCRIMINATOR AGAINST §2.6's [16] `PI`.
 * Both [23] and [77] continue with a component that BEGINS with a required [3] S — [24], [80] and [32] each
 * open with one — so there is no `<?xml?>` in either grammar. A `<?xml` NOT followed by white space is
 * therefore never a declaration, and what it is instead is decided by §2.6: [17] `PITarget ::= Name -
 * (('X' | 'x') ('M' | 'm') ('L' | 'l'))` keeps scanning [4a] `NameChar`s, so `<?xml-stylesheet ...?>` and
 * `<?xmls?>` are ordinary processing instructions with four-or-more-character targets, while `<?xml?>` has
 * the target `xml` exactly and is §2.6's reserved-target fatal error. core/xml/xml_markup.h states this
 * boundary from its own side — "a caller in §2.8's prolog owes [23] XMLDecl its own question first" — and the
 * two are held to each other by driving every declaration in this file's fixture through the PI scan as well.
 *
 * WHERE A DECLARATION MAY STAND IS THE CALLER'S RULE, NOT THIS COMPONENT'S, exactly as §1.2 Terminology's
 * "MUST NOT continue normal processing" is. [22] puts `XMLDecl?` before `Misc*`, so an XMLDecl is at offset
 * zero of the document entity or nowhere; §4.3.1 says the text declaration "MUST NOT appear at any position
 * other than the beginning of an external parsed entity" and §4.3.3 makes that a fatal error in as many
 * words. A reader positioned anywhere else still SCANS here, because the grammar of the construct does not
 * depend on where it stands — and the caller that knows which entity this is and how far into it the reader
 * has come is the one that owes the position check.
 *
 * NEITHER DOES IT DECIDE THE ENCODING, AND THAT IS THE SAME DIVISION core/xml/xml_char.h ALREADY MAKES. That
 * file is handed UTF-8 with §4.3.3's Byte Order Mark already consumed, because the encoding was determined
 * before a character was read; the `EncName` this scan hands back is what the DOCUMENT SAYS, which is a
 * different fact and is sometimes a contradicting one. §4.3.3: "it is a fatal error for an entity including
 * an encoding declaration to be presented to the XML processor in an encoding other than that named in the
 * declaration", and "It is a fatal error when an XML processor encounters an entity with an encoding that it
 * is unable to process." Reconciling the declared name against the encoding the bytes were actually decoded
 * from is the obligation of whichever step determined that encoding — it is the only one holding both facts —
 * and §4.3.3's own "XML processors SHOULD match character encoding names in a case-insensitive way" is that
 * step's rule too, which is why the slice here is returned exactly as the author wrote it and is not folded.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED SCAN CONSUMES NOTHING — both for
 * core/xml/xml_ref.h's reasons (a malformed document is a page's INPUT; §2.1 Well-Formed XML Documents makes
 * matching the grammar the first condition of well-formedness and §1.2 Terminology makes a violated
 * well-formedness constraint a fatal error; and the position a report quotes must name the CONSTRUCT rather
 * than some place inside the one that failed). THE ONE CARVE-OUT IS THE SAME ONE: an error the character
 * layer latched is NOT rewound, because restoring a saved reader would restore its §1.2 latch to XML_CHAR_OK
 * and silently un-report the fatal error that layer just detected.
 *
 * NO ALLOCATION, AND THE SCAN'S WHOLE STATE IS ITS READER. `VersionNum` and `EncName` come back as BORROWED
 * slices of the entity, and both are BYTE-EXACT rather than merely borrowed for core/xml/xml_markup.h's
 * argument about [17] `PITarget`: §2.11 End-of-Line Handling only ever rewrites #xD, and #xD is a decimal
 * digit in neither [26] nor a Latin letter in [81], so no character of either value can differ from the bytes
 * it was decoded from. That is what makes the reader-copy both the peek and the park here as everywhere else
 * in this family: a parse suspended in the prolog holds a copy of the reader at the construct's first byte
 * and resumes by scanning it again, with nothing outside the reader to restore. */
#ifndef APICLIENT_XML_DECL_H
#define APICLIENT_XML_DECL_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"

/* WHICH SENTENCE OF THE STANDARD THIS DECLARATION VIOLATED. One value per sentence, for core/xml/xml_ref.h's
   reason: an author has to be told which mistake was made, and a report that merged two would send them to the
   wrong one. `standalone` in a [77] TextDecl and a `standalone` whose value is neither string are two
   different mistakes, and so are a missing [24] VersionInfo and a `VersionNum` that is not `'1.' [0-9]+`.
   Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_DECL_OK = 0,
    XML_DECL_ERR_SPACE,                    /* [24]/[80]/[32] each open with a required [3] S */
    XML_DECL_ERR_VERSION_MISSING,          /* [23]: VersionInfo is not optional, and comes first */
    XML_DECL_ERR_VERSION_NUM,              /* [26] VersionNum ::= '1.' [0-9]+ */
    XML_DECL_ERR_ENCODING_MISSING,         /* [77]: EncodingDecl is not optional */
    XML_DECL_ERR_ENCODING_NAME,            /* [81] EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')* */
    XML_DECL_ERR_STANDALONE_VALUE,         /* [32]: the value is 'yes' or 'no' and there is no third */
    XML_DECL_ERR_STANDALONE_IN_TEXT_DECL,  /* [77] has no SDDecl — §2.9 is a component of [23] alone */
    XML_DECL_ERR_EQ,                       /* [25] Eq ::= S? '=' S? */
    XML_DECL_ERR_QUOTE,                    /* a value is `("'" X "'" | '"' X '"')` and this one is not */
    XML_DECL_ERR_COMPONENT,                /* what stands here is neither the next component nor the '?>' */
    XML_DECL_ERR_UNTERMINATED,             /* [23]/[77] close with '?>' and the entity ended first */
    XML_DECL_ERR_CHARACTER                 /* the character layer latched one — ask xml_char_error_message */
} XmlDeclError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_DECL_OK has a
   message too, and a caller that formats it has asked the wrong question. XML_DECL_ERR_CHARACTER's message
   says to ask the reader, because §2.2's and §4.3.3's character sentences are core/xml/xml_char.h's to word
   and a second copy here is how two spellings drift apart. */
const char *xml_decl_error_message(XmlDeclError err);

/* §2.9's THREE ANSWERS, AND THE ABSENT ONE IS A POSITIVE STATEMENT RATHER THAN A DEFAULT. §2.9: "If there are
   external markup declarations but there is no standalone document declaration, the value "no" is assumed"
   — and, in the sentence before it, "If there are no external markup declarations, the standalone document
   declaration has no meaning." So an absent [32] is NOT the same fact as `standalone='no'`: the assumption
   holds only once the caller knows there ARE external markup declarations, and that is §2.8's [28]
   doctypedecl's question rather than this one. Collapsing the two here would answer it before it was asked.
     It matters in exactly one place and it is the reason this file exists: §4.1's `[WFC: Entity Declared]`
   asks about "a document with `standalone='yes'`", so XML_STANDALONE_YES is a condition of that constraint
   while ABSENT and NO are both merely not-that. */
typedef enum {
    XML_STANDALONE_ABSENT = 0,   /* [23] carried no [32] SDDecl, and [77] never may */
    XML_STANDALONE_YES,
    XML_STANDALONE_NO
} XmlStandalone;

/* WHAT THE DECLARATION SAID. `version` is [26] `VersionNum` and `encoding` is [81] `EncName`, each BORROWED
   from the entity and byte-exact (see the head comment). An ABSENT component is a NULL pointer and not an
   empty slice, because neither production can match the empty string — `version=""` is XML_DECL_ERR_VERSION_NUM
   — so NULL is unambiguous and a consumer that read it crashes rather than seeing a version nobody wrote.
     `version` is NULL only for a [77] TextDecl, `encoding` only for a [23] XMLDecl, and `standalone` is
   XML_STANDALONE_ABSENT for every TextDecl; each of those is asserted by the scan rather than left to a
   caller to remember. */
typedef struct {
    const char   *version;    size_t version_len;
    const char   *encoding;   size_t encoding_len;
    XmlStandalone standalone;
} XmlDecl;

/* DOES THE READER STAND AT `'<?xml'` FOLLOWED BY [3] S? THE PEEK IS THE CALLER'S AND THE SCAN ASSERTS IT,
   which is core/xml/xml_markup.h's arrangement: a caller at the head of an entity decides between a
   declaration, a comment, a processing instruction and the document element, and that decision is ITS grammar
   rule. ONE predicate serves both productions because [23] and [77] open identically — which of the two this
   is depends on WHICH ENTITY the reader is reading, a fact only the caller has.
     IT IS A BYTE COMPARE AND THAT IS EXACT, by core/xml/xml_name.h's argument for the colon: every character
   of `<?xml` and every alternative of [3] S is ASCII, so each is a single byte in UTF-8, and none of those
   bytes can occur as a CONTINUATION byte of some other code point because every continuation byte is
   0x80..0xBF. §1.2 Terminology's `match` performs no case folding, so `<?XML ` is not this delimiter and this
   predicate says so — which is correct twice over, since §2.6's [17] then makes `<?XML` the reserved target. */
bool xml_decl_at(const XmlCharReader *r);

/* SCAN ONE DECLARATION. `_xmldecl` is §2.8's [23], for the document entity; `_textdecl` is §4.3.1's [77], for
   an external parsed entity. The reader MUST stand where `xml_decl_at` is true, and standing anywhere else is
   a caller that has not peeked, which is a DCHECK and not a document to report about.
   `*out` is written ONLY when XML_DECL_OK is returned, and the reader is then positioned immediately after
   the closing `'?>'`. On any other answer `*out` is untouched and — except for XML_DECL_ERR_CHARACTER, see the
   head comment — the reader is byte-for-byte the one that was handed in. */
XmlDeclError xml_decl_scan_xmldecl(XmlCharReader *r, XmlDecl *out);
XmlDeclError xml_decl_scan_textdecl(XmlCharReader *r, XmlDecl *out);

#endif
