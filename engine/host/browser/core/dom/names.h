/* DOM §1.4 "Name validation" — WHICH STRINGS MAY BE AN ELEMENT'S LOCAL NAME.
 *
 * ITS OWN FILE BECAUSE IT IS ASKED FROM TWO STANDARDS. `createElement` throws InvalidCharacterError by it and
 * HTML §4.13.3 "Core concepts"'s "valid custom element name" is it plus four further requirements, so a copy
 * inside either one is a copy that drifts: the custom-element registry's was "the first byte is a-z and a hyphen
 * somewhere", which accepted `a-A`, accepted `annotation-xml`, and accepted every name whose later code points
 * the DOM forbids — 1704 subtests of one WPT file disagreeing with one four-line function.
 *
 * LENGTH-CARRYING, NOT NUL-TERMINATED, because U+0000 is a code point a page can put in a name and rejecting
 * it is half of what step 2.1 is for. `strlen` would answer about a prefix of the string.
 *
 * BYTES, NOT CODE POINTS, AND THAT IS EXACT. Every ASCII code point is one byte < 0x80 in UTF-8 and every
 * non-ASCII code point is a run of bytes >= 0x80, so "is an ASCII x" and "is in the range U+0080 to U+10FFFF"
 * are both byte tests — and a continuation byte of a permitted non-ASCII code point is itself >= 0x80, so a
 * byte walk accepts exactly the code-point walk's set. */
#ifndef APICLIENT_DOM_NAMES_H
#define APICLIENT_DOM_NAMES_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"   /* §1.4's steps THROW, so the component that owns them owns their exceptions too */

/* DOM §1.4 "valid element local name", the five steps exactly. Note that it is the ONLY one of the three that
   is an algorithm rather than a character set, and that it permits U+003D (=), which an attribute local name
   does not — §1.4's stated intention is "to allow any name that is possible to construct using the HTML PARSER,
   plus some additional possibilities", so a name XML 1.0 §2.3's `Name` production rejects (`a=b`, `a<b`, `A`
   followed by U+00D7) is valid here and setAttribute does not throw for it. The three examples that stood here
   were each WRONG in a way this file's own steps decide: `1abc` and `<` both fail step 3, which admits only
   U+003A, U+005F and U+0080 and above once step 2's ASCII-alpha branch is not taken, and `a:b` is a perfectly
   good Name because §2.3 requires a processor to accept the colon as a name character. The two predicates are
   ORDERED — every Name is a valid element local name, never the reverse — and core/xml/xml_name.h, which owns
   the `Name` production the DOM references for createProcessingInstruction, states that ordering from its side. */
bool dom_valid_element_local_name(const char *name, size_t len);
/* DOM §1.4 "valid namespace prefix": length >= 1, and no ASCII whitespace, U+0000, U+002F (/) or U+003E (>).
   Note what is NOT there: U+003D (=), which an attribute local name forbids and a prefix does not. */
bool dom_valid_namespace_prefix(const char *name, size_t len);
/* DOM §1.4 "valid attribute local name": the prefix set plus U+003D (=). */
bool dom_valid_attribute_local_name(const char *name, size_t len);

/* WHICH KIND OF NAME IS BEING VALIDATED — §1.4's algorithm takes it as a parameter, because an ATTRIBUTE's
   local name and an ELEMENT's are different predicates: `a=b` is a legal element local name and an illegal
   attribute one. Passing the wrong one is a spec bug that no test of the other kind can see, which is why it
   is a named argument rather than a bool. */
typedef enum { DOM_NAME_ATTRIBUTE, DOM_NAME_ELEMENT } DomNameKind;

/* The result of §1.4 "validate and extract": three BORROWED slices, none NUL-terminated.
   Borrowed because they are slices OF the caller's qualifiedName — a prefix and a local name are the two halves
   either side of the colon, and copying them here would make every caller free something to learn where the
   colon was. `ns`/`prefix` are NULL when there is none, which is §1.4's null, not the empty string. */
typedef struct {
    const char *ns;     size_t ns_len;
    const char *prefix; size_t prefix_len;
    const char *local;  size_t local_len;
} DomQName;

/* DOM §1.4 "validate and extract a namespace and qualifiedName", the TWELVE steps exactly. Returns false with
   the DOMException the step names ALREADY THROWN — InvalidCharacterError for a name the productions reject,
   NamespaceError for a (prefix, namespace) pairing the standard forbids. A caller that only wants to know
   whether it may proceed reads the bool; the exception is the page's answer either way. */
bool dom_validate_and_extract(JSContext *ctx, const char *ns, size_t ns_len,
                              const char *qname, size_t qname_len, DomNameKind kind, DomQName *out);

#endif
