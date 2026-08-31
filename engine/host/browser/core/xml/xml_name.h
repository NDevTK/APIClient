/* XML 1.0 (Fifth Edition) §2.3's `Name`, AND Namespaces in XML 1.0's `NCName` and `QName`.
 *
 * WHAT IT IS. XML 1.0 §2.3's three productions — [4] NameStartChar, [4a] NameChar, and
 * [5] Name ::= NameStartChar (NameChar)* — and the three that Namespaces in XML 1.0 builds directly on top of
 * them: §3's [4] NCName, and §4's [7] QName with its [8] PrefixedName / [9] UnprefixedName split. It is the
 * LEAF of an XML parser and the piece everything above it stands on — every element name, attribute name,
 * processing-instruction target, entity name and notation name a tokenizer scans is one of these — which is why
 * it is its own translation unit at the bottom of core/xml/ rather than a static helper inside whichever file
 * needs it first.
 *
 * TWO STANDARDS IN ONE FILE, AND THEY CANNOT BE SEPARATED. Namespaces in XML defines its productions BY
 * SUBTRACTION FROM AND COMPOSITION OF XML 1.0's: "[4] NCName ::= Name - (Char* ':' Char*)", whose own inline
 * gloss reads "An XML Name, minus the colon", and "[10] Prefix ::= NCName", "[11] LocalPart ::= NCName". There
 * is no second character class anywhere in Namespaces in XML — there is XML 1.0's, read with one code point
 * excluded. Splitting them
 * would leave core/xml/xml_ncname.c holding no data and no test of its own while forcing NAME_START_CHAR and
 * NAME_CHAR_ONLY out of static scope, which is exactly the split core/fetch/port_blocking.c refuses and for
 * exactly its reason: the two files would only ever change together, and the tables would become a data header
 * with no algorithm attached — which is what core/url's idna_table.h and public_suffix_table.h are and what
 * three lines of hand-transcribed spec text are not.
 *
 * A NOTE ON WHERE THE PRODUCTIONS ACTUALLY LIVE, because the section numbers are easy to get wrong and the
 * queue that named this work did: NCName is production [4] of §3 "Declaring Namespaces", where it appears as
 * the tail of [2] PrefixedAttName; QName is production [7] of §4 "Qualified Names", a section later. The
 * namespace CONSTRAINTS that read them are elsewhere again — §3's Reserved Prefixes and Namespace Names, §5's
 * Prefix Declared and No Prefix Undeclaring, §6.3's Attributes Unique — and none of them is in this file,
 * because a constraint needs a scope and this file has no state. They are core/xml/xml_ns.h's.
 *
 * ITS CONSUMER TODAY IS THE DOM, NOT AN XML PARSER, and that is the standard's own arrangement rather than a
 * placeholder. DOM §4.13's "initialize a ProcessingInstruction node" step 1 is "if target does not match the
 * Name production, then throw an InvalidCharacterError", and the phrase "Name production" in that step links
 * to https://www.w3.org/TR/xml/#NT-Name — to THIS, not to any of the DOM's own three name predicates.
 *
 * IT IS NOT core/dom/names.c AND MUST NEVER MERGE WITH IT. Those are DOM §1.4's predicates, whose stated
 * intention is "to allow any name that is possible to construct using the HTML parser (the branch where the
 * first code point is an ASCII alpha), plus some additional possibilities". The two are ORDERED, and the order is worth checking rather than assuming: every Name is a
 * valid element local name (a Name's first code point is `:`, `_`, an ASCII alpha or something >= U+00C0, and
 * every NameChar after it is an ASCII alpha, an ASCII digit, `-`, `.`, `:`, `_` or >= U+0080 — which is §1.4
 * step 4's set exactly), while the converse fails wide open: `a=b`, `a<b` and `A` followed by U+00D7 are all
 * valid element local names and none of them is a Name. So one predicate serving both would be LOOSE in one
 * direction and could only be made tight by breaking the other — createProcessingInstruction would stop
 * throwing on names XML forbids, or createElement would start throwing on names the HTML parser hands it.
 *
 * CODE POINTS, NOT BYTES, AND THAT IS EXACTLY WHERE IT DIVERGES FROM names.c. names.h can walk BYTES, and its
 * head comment proves the walk exact, because §1.4 permits EVERY code point >= U+0080: "is this byte >= 0x80"
 * is then the whole test, and a UTF-8 continuation byte answers it the same way its lead byte does. XML permits
 * no such thing. U+00B7 MIDDLE DOT is a NameChar and is NOT a NameStartChar; U+00D7 MULTIPLICATION SIGN is
 * neither, sitting in the hole [4] leaves between [#xC0-#xD6] and [#xD8-#xF6]. Both are >= 0x80, so a byte walk
 * accepts both of them in both positions. Four subtests of wpt/dom/nodes/Document-createProcessingInstruction.js
 * are those two code points and nothing else — U+00B7 leading, U+00D7 leading and U+00D7 trailing are all
 * invalid, against a trailing U+00B7 which is valid — so the byte shortcut is not a smaller version of this
 * file, it is a wrong one.
 *
 * LENGTH-CARRYING, NOT NUL-TERMINATED, for names.h's reason: U+0000 is a code point a page can put in a
 * processing-instruction target, it is in no range of either production, and `strlen` would answer about a
 * prefix of the string instead of about the name. */
#ifndef APICLIENT_XML_NAME_H
#define APICLIENT_XML_NAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* XML 1.0 §2.3's [4] `NameStartChar` and [4a] `NameChar`, ONE CODE POINT AT A TIME — the two productions [5]
   `Name` is composed of, asked the way a SCANNER has to ask them.
     They are exposed rather than private to this file because a tokenizer reading a name out of a stream
   cannot use the slice predicate below: it does not yet know where the name ENDS, and finding out IS asking
   [4a] of each character until one says no. core/xml/xml_ref.c's [68] `EntityRef ::= '&' Name ';'` is the
   first caller and every other name in the grammar — an element type, an attribute, a PI target — is scanned
   the same way. The slice predicate and these two are ONE transcription read two ways, and xml_ref.c asserts
   they agree on every name it scans.
     They take a CODE POINT, not bytes, for the reason this header's opening gives: [4] has a hole at U+00D7
   and U+00B7 is [4a]-only, so no byte test can stand in for either. */
bool xml_name_is_name_start_char(uint32_t cp);
bool xml_name_is_name_char(uint32_t cp);

/* XML 1.0 §2.3 [5] `Name`. `s` is `len` bytes of WELL-FORMED UTF-8 — the JS string encoder's output — so a
   truncated or structurally invalid sequence is an engine bug that crashes at the walk rather than a name this
   answers about. */
bool xml_name_is_name(const char *s, size_t len);

/* Namespaces in XML 1.0 §3 [4] `NCName ::= Name - (Char* ':' Char*)` — a Name with no colon ANYWHERE, not a
   Name whose first character is not a colon. The subtracted language is every string with a colon in it, so
   `a:b` is excluded by its interior colon and not by its start.

   IT IS A BYTE TEST FOR THE COLON AND A CODE-POINT WALK FOR THE REST, and the halves are exact for different
   reasons. U+003A is ASCII, so in UTF-8 it is the single byte 0x3A and — this is the load-bearing half — that
   byte can never occur as a CONTINUATION byte of some other code point, because every continuation byte is
   0x80..0xBF. So "does this string contain U+003A" and "does this byte run contain 0x3A" are the same question,
   which is why core/dom/names.h's byte-walk argument applies to the colon and to nothing else here: every
   OTHER distinction Namespaces in XML inherits is XML 1.0's character classes, which have holes above U+007F
   (U+00D7) and therefore have to be walked as code points. */
bool xml_name_is_ncname(const char *s, size_t len);

/* Namespaces in XML 1.0 §4 [7] `QName`, split into the [10] `Prefix` and [11] `LocalPart` the caller needs.
   `prefix` is NULL for [9] UnprefixedName — the standard's absence, not the empty string, since `:b` and `b`
   are different strings and only the second is a QName. Both slices are BORROWED from `s`. */
typedef struct {
    const char *prefix; size_t prefix_len;
    const char *local;  size_t local_len;
} XmlQName;

/* [7] QName ::= PrefixedName | UnprefixedName, where [8] PrefixedName ::= Prefix ':' LocalPart and both halves
   are NCNames. Returns false and WRITES NOTHING when `s` is not a QName — a half-filled out-parameter is a
   producer's field defaulted, which is worse than no answer.

   THIS IS NOT DOM §1.4's SPLIT, AND THE DIFFERENCE IS OBSERVABLE. validate-and-extract splits at the FIRST
   colon and keeps everything after it as the local name, so `a:b:c` is prefix `a` and local name `b:c` there
   and `document.createElementNS(ns, "a:b:c")` builds an element. Here the same string is NOT a QName at all,
   because `b:c` is not an NCName — an XML document containing `<a:b:c/>` is namespace-ill-formed. Both are
   right for their own standard; core/dom/names.h states the ordering from its side. */
bool xml_name_parse_qname(const char *s, size_t len, XmlQName *out);

#endif
