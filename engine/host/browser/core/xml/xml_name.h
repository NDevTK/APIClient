/* XML 1.0 (Fifth Edition) §2.3 — THE `Name` PRODUCTION.
 *
 * WHAT IT IS. Three productions and nothing else: [4] NameStartChar, [4a] NameChar, and
 * [5] Name ::= NameStartChar (NameChar)*. It is the LEAF of an XML parser and the piece everything above it
 * stands on — every element name, attribute name, processing-instruction target, entity name and notation name
 * a tokenizer scans is this production — which is why it is its own translation unit at the bottom of
 * core/xml/ rather than a static helper inside whichever file needs it first.
 *
 * ITS CONSUMER TODAY IS THE DOM, NOT AN XML PARSER, and that is the standard's own arrangement rather than a
 * placeholder. DOM §4.13's "initialize a ProcessingInstruction node" step 1 is "if target does not match the
 * Name production, then throw an InvalidCharacterError", and the phrase "Name production" in that step links
 * to https://www.w3.org/TR/xml/#NT-Name — to THIS, not to any of the DOM's own three name predicates.
 *
 * IT IS NOT core/dom/names.c AND MUST NEVER MERGE WITH IT. Those are DOM §1.4's predicates, whose stated
 * intention is "to allow any name that is possible to construct using the HTML parser, plus some additional
 * possibilities". The two are ORDERED, and the order is worth checking rather than assuming: every Name is a
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

/* XML 1.0 §2.3 [5] `Name`. `s` is `len` bytes of WELL-FORMED UTF-8 — the JS string encoder's output — so a
   truncated or structurally invalid sequence is an engine bug that crashes at the walk rather than a name this
   answers about. */
bool xml_name_is_name(const char *s, size_t len);

#endif
